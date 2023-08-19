#pragma once

#include <stdint.h>
#include <assert.h>


namespace natKit {

struct EventData {
    virtual ~EventData() = default;
};

struct NoEventData : public EventData {};

struct StateMapRow;

class MicroStateMachine {
public:
    MicroStateMachine(uint8_t maxNumberOfStates, uint8_t initalState = 0)
      : maxNumberOfStates(maxNumberOfStates), currentState(initalState) {}

    virtual ~MicroStateMachine() {}

    uint8_t getCurrentState() { return currentState; }

protected:
    enum {
        EVENT_IGNORED=0xFE,
        CANNOT_HAPPEN
    };

    uint8_t currentState;
    void processExternalEvent(uint8_t state, EventData* data = nullptr) {
        if (state == EVENT_IGNORED) {
            if (data) {
                delete data;
            }
        } else {
            processInternalEvent(state, data);
            run();
        }
    }

    void processInternalEvent(uint8_t state, EventData* data = nullptr) {
        if (data == nullptr) {
            data = new EventData();
        }
        eventData = data;
        isEventGenerated = true;
        currentState = state;
    }

    virtual const StateMapRow* getStateMap() = 0;

private:
    const uint8_t maxNumberOfStates;
    uint8_t nextState;
    bool isEventGenerated;
    EventData* eventData;
    
    void setCurrentState(uint8_t state) { currentState = state; }

    void run();   
};

typedef void (MicroStateMachine::*StateFunc)(EventData *);
struct StateStruct 
{
    StateFunc stateFunc;    
};

}
 
#define BEGIN_STATE_MAP \
public:\
const StateMapRow* getStateMap() {\
    static const StateStruct stateMap[] = { 
 
#define STATE_MAP_ENTRY(__stateFunc)\
    { reinterpret_cast<StateFunc>(__stateFunc) },
 
#define END_STATE_MAP \
    }; \
    return &stateMap[0]; }
 
#define BEGIN_TRANSITION_MAP \
    static const unsigned char TRANSITIONS[] = {\
 
#define TRANSITION_MAP_ENTRY(__entry)\
    __entry,
 
#define END_TRANSITION_MAP(__data) \
    0 };\
    processExternalEvent(TRANSITIONS[currentState], __data);
