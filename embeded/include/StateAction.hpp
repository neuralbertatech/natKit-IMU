#pragma once

#include <MicroStateMachine.hpp>

namespace natKit {

class StateAction {
public:
    virtual void invokeStateAction(MicroStateMachine* stateMachine, const EventData* data) const = 0;
};

}