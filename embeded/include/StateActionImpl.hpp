#pragma once

#include <StateAction.hpp>

namespace natKit {

template <class StateMachine, class Data, void (StateMachine::*Func)(const Data*)>
class StateActionImpl : public StateAction {
public:
    virtual void invokeStateAction(MicroStateMachine* stateMachine, const EventData* data) const override {
        // StateMachine* derivedStateMachine = static_cast<StateMachine*>(stateMachine);
        // const Data* derivedEventData = dynamic_cast<const Data*>(data);

        // assert(derivedEventData != nullptr);
        // (derivedStateMachine->(*Func))(derivedEventData);
    }
};

}