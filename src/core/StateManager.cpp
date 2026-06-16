#include "FluxPlayer/core/StateManager.h"
#include "FluxPlayer/utils/Logger.h"

#include <string>

namespace FluxPlayer {

void StateManager::transitionTo(PlayerState newState) {
    PlayerState oldState = state_.exchange(newState);

    if (oldState != newState) {
        LOG_INFO("Player state changed: " +
                std::to_string(static_cast<int>(oldState)) + " -> " +
                std::to_string(static_cast<int>(newState)));

        if (callback_) {
            callback_(newState);
        }
    }
}

} // namespace FluxPlayer
