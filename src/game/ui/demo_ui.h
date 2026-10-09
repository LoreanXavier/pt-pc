#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/ui/uia.h"
#include "engine/ui/uigb.h"
#include "engine/ui/uilb.h"
#include "game/ui/uia_player.h"
#include "game/ui/uif_view.h"

namespace pt::game {

class Game;

class DemoUi {
public:
    static constexpr int kPriority = 181;

    void Init(UiAssets& assets) { assets_ = &assets; }
    void Update(Game& game, float dt, bool paused);
    void Draw(ui::UiBatch& batch, const UiCanvas& canvas, int language);
    bool Active() const { return !instances_.empty(); }
    void Clear() {
        instances_.clear();
        bug_graph_ = 0;
    }
    // The fake crash's bug picture is up and its setin has finished: the original stops presenting new frames until the setout
    // (floor_f120 3900 to 4130: every pixel of every frame identical, no film grain, the scene at the sides frozen)
    bool BugScreenStill() const;

    bool Create(std::string_view demo_id, uint64_t graph, std::string_view path);
    bool Start(uint64_t graph);
    // `bug_screen` 0..6 takes the place of the pick of the random bug screen (GameConfig::bug_screen; 6 is the port's seventh)
    bool Text(uint64_t graph, uint64_t text, int bug_screen = -1);

private:
    struct ModelView {
        const ui::UifModel* model = nullptr;
        std::unique_ptr<UifView> view;
        UiaPlayers players;
        std::vector<std::optional<bool>> visible;
        std::vector<uint64_t> animations;
    };
    struct Layout {
        uint64_t name = 0;
        std::string path;
        std::shared_ptr<const ui::UilbLayout> data;
        std::vector<ModelView> models;
        bool visible = true;
    };
    struct Instance {
        std::string demo_id;
        uint64_t graph = 0;
        std::shared_ptr<const ui::UigbGraph> data;
        std::vector<Layout> layouts;
        bool started = false;
    };

    Instance* Find(uint64_t graph);
    void Run(Instance& instance, const std::vector<ui::UigbAction>& actions);
    void Play(Instance& instance, const ui::UigbAction& action);
    void SetVisible(Instance& instance, const ui::UigbAction& action);
    void Apply(ModelView& model);
    const ui::UiaAnimation* Animation(const std::string& path);
    std::shared_ptr<const ui::UilbLayout> LoadLayout(const std::string& path);
    std::shared_ptr<const ui::UigbGraph> LoadGraph(const std::string& path);

    UiAssets* assets_ = nullptr;
    std::vector<std::unique_ptr<Instance>> instances_;
    uint64_t bug_graph_ = 0;
    float bug_shown_ = 0.0f;
    std::map<std::string, std::unique_ptr<ui::UiaAnimation>> animations_;
    std::map<std::string, std::shared_ptr<const ui::UilbLayout>> layouts_;
    std::map<std::string, std::shared_ptr<const ui::UigbGraph>> graphs_;
};

}
