#ifndef _EVENT_SENDER_H_
#define _EVENT_SENDER_H_
#include "EventMessage.h"

class EventSender
{
        public:
                virtual bool sendMessage(EventMessage &) = 0;
};
#endif
