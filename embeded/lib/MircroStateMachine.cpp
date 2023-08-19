#include <MicroStateMachine.hpp>
#include <StateMapRow.hpp>


namespace natKit {

void MicroStateMachine::run() {
    EventData* tmpEventData = nullptr;
    while (isEventGenerated) {
        tmpEventData = eventData;
        eventData = nullptr;
        isEventGenerated = false;

        assert(currentState < maxNumberOfStates);

        const StateMapRow* stateMap = getStateMap();
        stateMap[currentState].state->invokeStateAction(this, tmpEventData);

        if (tmpEventData) {
            delete tmpEventData;
            tmpEventData = nullptr;
        }
    }
}  

}