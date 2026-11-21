#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pt::audio {
class Media;
}

namespace pt::game::survival_sounds {

using Sound = std::shared_ptr<const audio::Media>;

Sound FromFile(std::string_view name);
std::vector<Sound> FromFilePool(std::string_view name);
std::map<std::string, Sound, std::less<>> FromFolder(std::string_view folder);

struct Take {
    Sound media;
    std::shared_ptr<const std::vector<float>> pcm;
    uint32_t channels = 1;
};
Take Hold(Sound media, bool decode);
Sound Varied(const Take& take, float gain, float speed);

Sound Gunshot(float thump_hz, float thump_decay, float blast_decay, float blast_hz, float tail, float peak, unsigned seed);
Sound DryFire();
inline constexpr float kReloadPace = 1.4f;
Sound Reload(bool empty);
Sound Pump();
Sound ShellIn();
Sound RevolverReload();
Sound Casing(bool shell);
Sound BulletImpact();
Sound ArmorDeflect();
Sound GlassBreak();
Sound BodyFall();
Sound HeavyThud();
Sound PickupLand();
Sound PickupTake();
Sound HealTake();
Sound Swap();
Sound Drop();
Sound Swing();
Sound Siren();
Sound Tick(bool last);

Sound RadioTakeOne(const audio::Media* riddle, const audio::Media* voice08, const audio::Media* noise, int variant);

}
