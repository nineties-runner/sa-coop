#pragma once
#include <cstdint>

namespace game {
struct CVehicle;
}

namespace coop {
struct RemotePlayer;

namespace radio {

constexpr int8_t RADIO_USER_TRACKS = 12;
constexpr int8_t RADIO_OFF = 13;

bool CarRadio();
bool CarRadio(game::CVehicle* v);
const RemotePlayer* RemoteDriver();
int8_t RequestedStation();
int8_t ActiveStation();
void TuneToUserTracks();
bool ShouldMuteNativeRadio();
bool IsDriverOnUserTracks();
bool IsPassengerOnUserTracks();
float GameRadioVolume();

}  // namespace radio
}  // namespace coop
