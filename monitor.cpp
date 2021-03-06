#include <iostream>
#include <exception>
#include <stdio.h>
#include <signal.h>
#include <atomic>
#include <mutex>
#include <pthread.h>

#include <tins/tins.h>

#define CLEANUP_PERIOD 30
#define CLEANUP_MAXAGE 4


using namespace Tins;

#include "ClientInfo.h"
#include "Parser.h"
#include "RadioTapParser.h"
#include "Dot11Parser.h"
#include "ClientDb.h"
#include "ApDb.h"
#include "ZmqEventSender.h"

struct cleanup_thread_params {
        ClientDb *clt_db;
        ApDb *ap_db;
        std::atomic_bool cleanup_thread_flag;
} thread_params;

void *cleanup_function(void *param)
{
        struct cleanup_thread_params *params = (struct cleanup_thread_params *)param;

        while (params->cleanup_thread_flag.load()) {
                sleep(CLEANUP_PERIOD);
                params->clt_db->cleanup(CLEANUP_MAXAGE);
                params->ap_db->cleanup(CLEANUP_MAXAGE);
        }
}

pthread_t cleanup_thread;
std::once_flag terminate_flag;

std::vector<Parser *> parsers;
ClientDb *clientdb;
ApDb *apdb;
BaseSniffer *sniffer;
ZmqEventSender *sender;

static int count;
static int ignored;

void terminate_function()
{
        std::cerr << "Terminating, waiting for cleanup thread to finish\n";
        thread_params.cleanup_thread_flag.store(false);
        pthread_cancel(cleanup_thread);
        pthread_join(cleanup_thread, NULL);

        for (std::vector<Parser *>::iterator it = parsers.begin() ; it != parsers.end(); ++it) {
                Parser *parser = *it;
                delete parser;
        }

        std::cerr << "Finishing, total packets: " << count << " ignored: " << ignored << std::endl;
        std::cerr << *clientdb << std::endl;
        std::cerr << *apdb << std::endl;
        delete clientdb;
        delete apdb;
        delete sender;
        delete sniffer;
}

void terminate_handler(int sig)
{
        std::call_once(terminate_flag, terminate_function);
        /* re-raise */
        signal(sig, SIG_DFL);
        raise(sig);
}

bool parse(const RefPacket &ref)
{
        ClientInfo info;

        count++;
        info.timestamp = ref.timestamp();
        for (std::vector<Parser *>::iterator it = parsers.begin() ; it != parsers.end(); ++it) {
                if (!(*it)->parse(&info, ref.pdu()))
                        break;
        }

        if (!info.interesting) {
                ignored++;
        }


        return true;
}


int main(int argc, char **argv)
{
        int ret;
        sigset_t signal_mask;
        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, SIGINT);
        char report_dev[32];

        if (argc == 4 && !strcmp(argv[1], "-f")) {
                sniffer = new FileSniffer(std::string(argv[2]));
                sprintf(report_dev, "tcp://%s:*", argv[3]);
        } else if (argc == 3) {
                sniffer = new Sniffer(std::string(argv[1]), 1500, true);
                sprintf(report_dev, "tcp://%s:*", argv[2]);
        } else {
                std::cerr << "Usage: " << argv[0] << " <monitor device> <report device>" << std::endl;
                exit(1);
        }

        if (!sniffer)
                exit(1);

        ret = pthread_sigmask (SIG_BLOCK, &signal_mask, NULL);
        if (ret != 0)
                std::cerr << "failed to set sigmask\n";

        sender = new ZmqEventSender();
        if (!sender->bind(report_dev))
                exit(1);

        clientdb = new ClientDb(sender);
        apdb = new ApDb(sender);

        thread_params.clt_db = clientdb;
        thread_params.ap_db = apdb;
        thread_params.cleanup_thread_flag.store(true);
        ret = pthread_create(&cleanup_thread, NULL, cleanup_function, (void *)&thread_params);
        if (ret)
                std::cerr << "failed to create cleanup thread\n";

        parsers.push_back(new RadioTapParser());
        parsers.push_back(new Dot11ApParser(apdb));
        parsers.push_back(new Dot11StaParser(clientdb, apdb));

        /* Ctrl-C handler */
        ret = pthread_sigmask (SIG_UNBLOCK, &signal_mask, NULL);
        if (ret != 0)
                std::cerr << "failed to set sigmask\n";

        signal(SIGINT, terminate_handler);

        sniffer->sniff_loop(parse);

        std::cerr << "sniff_loop exited\n";
        terminate_function();
        exit(1);
}
