#include "game/ui/demo_ui.h"

#include <algorithm>
#include <cmath>

#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "engine/fs/vfs.h"
#include "game/demo_system.h"
#include "game/game.h"

namespace pt::game {
namespace {

constexpr uint64_t kTextFunctor = 0xECEC4D259E21;
constexpr uint64_t kRandomBugText = 0x90BA012E9D3E;
constexpr int kBugCases = 7;
constexpr uint64_t kBugTexts[6] = {0x119CABD7B66A, 0x69D97AC6AF45, 0xC19FC491E18F, 0xF89D156E5DA1, 0xED7A96A7C6B6, 0xA957904C892D};
constexpr uint64_t kBug3Mesh = 0x8F1B33D1D824;
constexpr const char* kBugPictures[kBugCases] = {"sh_bug_1, grey, mirrored lines", "sh_bug_2, black, Fix this damn bug",
                                                 "sh_bug_4, white, Knowing you ... -J", "sh_bug_5, red, I'm heading there now",
                                                 "sh_bug_6, yellow, I'll call later", "sh_bug_7, black, This game is purely fictitious",
                                                 "sh_bug_3, yellow, Release the game for free (port addition)"};

std::string_view Stem(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

}

std::shared_ptr<const ui::UigbGraph> DemoUi::LoadGraph(const std::string& path) {
    if (auto it = graphs_.find(path); it != graphs_.end()) {
        return it->second;
    }
    auto data = assets_->Files().ReadFile(path);
    auto graph = std::make_shared<ui::UigbGraph>();
    std::string error;
    if (!data || !graph->Parse(*data, &error)) {
        LogError("ui: graph {}: {}", path, data ? error : std::string("not found"));
        return nullptr;
    }
    graphs_[path] = graph;
    return graph;
}

std::shared_ptr<const ui::UilbLayout> DemoUi::LoadLayout(const std::string& path) {
    if (auto it = layouts_.find(path); it != layouts_.end()) {
        return it->second;
    }
    auto data = assets_->Files().ReadFile(path);
    auto layout = std::make_shared<ui::UilbLayout>();
    std::string error;
    if (!data || !layout->Parse(*data, &error)) {
        LogError("ui: layout {}: {}", path, data ? error : std::string("not found"));
        return nullptr;
    }
    layouts_[path] = layout;
    return layout;
}

const ui::UiaAnimation* DemoUi::Animation(const std::string& path) {
    if (auto it = animations_.find(path); it != animations_.end()) {
        return it->second.get();
    }
    auto data = assets_->Files().ReadFile(path);
    auto animation = std::make_unique<ui::UiaAnimation>();
    std::string error;
    if (!data || !animation->Parse(*data, &error)) {
        LogError("ui: animation {}: {}", path, data ? error : std::string("not found"));
        return nullptr;
    }
    const ui::UiaAnimation* out = animation.get();
    animations_[path] = std::move(animation);
    return out;
}

DemoUi::Instance* DemoUi::Find(uint64_t graph) {
    for (auto& instance : instances_) {
        if (instance->graph == graph) {
            return instance.get();
        }
    }
    return nullptr;
}

bool DemoUi::Create(std::string_view demo_id, uint64_t graph, std::string_view path) {
    if (!assets_ || path.empty()) {
        LogWarn("ui: demo {} graph {:#x} without a file", demo_id, graph);
        return false;
    }
    auto data = LoadGraph(std::string(path));
    if (!data) {
        return false;
    }
    auto instance = std::make_unique<Instance>();
    instance->demo_id = std::string(demo_id);
    instance->graph = graph;
    instance->data = data;
    for (const std::string& layout_path : data->Layouts()) {
        Layout layout;
        layout.path = layout_path;
        layout.name = StrCode64(Stem(layout_path));
        layout.data = LoadLayout(layout_path);
        if (layout.data) {
            for (const ui::UilbModel& m : layout.data->Models()) {
                ModelView view;
                view.model = assets_->Model(m.path);
                if (!view.model) {
                    continue;
                }
                view.view = std::make_unique<UifView>();
                view.view->Bind(view.model, assets_);
                view.visible.assign(view.model->Nodes().size(), std::nullopt);
                view.animations = m.animations;
                layout.models.push_back(std::move(view));
            }
        }
        instance->layouts.push_back(std::move(layout));
    }
    std::erase_if(instances_, [&](const auto& i) { return i->graph == graph; });
    LogInfo("ui: demo {} graph {:#x} {} ({} layouts)", demo_id, graph, path, instance->layouts.size());
    instances_.push_back(std::move(instance));
    return true;
}

bool DemoUi::Start(uint64_t graph) {
    Instance* instance = Find(graph);
    if (!instance) {
        LogWarn("ui: demo graph {:#x} start before create", graph);
        return false;
    }
    instance->started = true;
    Run(*instance, instance->data->StartActions());
    return true;
}

bool DemoUi::Text(uint64_t graph, uint64_t text, int bug_screen) {
    Instance* instance = Find(graph);
    if (!instance) {
        LogWarn("ui: demo graph {:#x} text {:#x} with no graph", graph, text);
        return false;
    }
    std::vector<ui::UigbAction> added;
    if ((text & kStrCode64Mask) == kRandomBugText) {
        uint32_t x = ReadTsc();
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 5;
        const uint32_t pick = bug_screen >= 0 && bug_screen < kBugCases ? static_cast<uint32_t>(bug_screen) : x % kBugCases;
        LogInfo("ui: bug screen {} of {} ({}){}", pick, kBugCases, kBugPictures[pick], bug_screen >= 0 ? ", forced" : "");
        if (pick == 6) {
            if (const ui::UigbEvent* model = instance->data->FindEvent(kBugTexts[0])) {
                added = model->actions;
                for (ui::UigbAction& action : added) {
                    if (action.kind == ui::UigbActionKind::SetVisible) {
                        action.visible = action.target == kBug3Mesh;
                    }
                }
                LogInfo("ui: demo graph {:#x} event sh_bug_3 ({} actions)", graph, added.size());
                Run(*instance, added);
                return true;
            }
        }
        text = kBugTexts[pick < 6 ? pick : 0];
    }
    const ui::UigbEvent* event = instance->data->FindEvent(text);
    if (!event) {
        LogWarn("ui: demo graph {:#x} has no event {:#x}", graph, text);
        return false;
    }
    LogInfo("ui: demo graph {:#x} event {:#x} ({} actions)", graph, text, event->actions.size());
    Run(*instance, event->actions);
    return true;
}

void DemoUi::Run(Instance& instance, const std::vector<ui::UigbAction>& actions) {
    for (const ui::UigbAction& action : actions) {
        if (action.kind == ui::UigbActionKind::PlayAnimation) {
            Play(instance, action);
        } else {
            SetVisible(instance, action);
        }
    }
}

void DemoUi::Play(Instance& instance, const ui::UigbAction& action) {
    for (Layout& layout : instance.layouts) {
        const ui::UilbAnimation* animation = layout.data ? layout.data->FindAnimation(action.animation) : nullptr;
        if (!animation) {
            continue;
        }
        std::vector<const ui::UiaAnimation*> parts;
        for (const std::string* path : {&animation->main, &animation->shader}) {
            if (!path->empty()) {
                if (const ui::UiaAnimation* a = Animation(*path)) {
                    parts.push_back(a);
                }
            }
        }
        const bool listed = std::any_of(layout.models.begin(), layout.models.end(),
                                        [&](const ModelView& m) { return std::ranges::find(m.animations, animation->name) != m.animations.end(); });
        for (ModelView& model : layout.models) {
            if (listed && std::ranges::find(model.animations, animation->name) == model.animations.end()) {
                continue;
            }
            for (const ui::UiaAnimation* a : parts) {
                model.players.Play(a, animation->speed * action.speed, action.loop);
            }
        }
        return;
    }
    LogWarn("ui: demo graph animation {:#x} not in its layouts", action.animation);
}

void DemoUi::SetVisible(Instance& instance, const ui::UigbAction& action) {
    for (Layout& layout : instance.layouts) {
        if (layout.name != action.layout) {
            continue;
        }
        if (action.target == layout.name || action.target == ui::UigbGraph::kEmptyName) {
            layout.visible = action.visible;
            return;
        }
        for (ModelView& model : layout.models) {
            const auto& nodes = model.model->Nodes();
            const auto& names = model.model->Names();
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].id < names.size() && names[nodes[i].id] == action.target) {
                    model.visible[i] = action.visible;
                }
            }
        }
        return;
    }
}

void DemoUi::Apply(ModelView& model) {
    model.view->ClearAnimation();
    for (size_t i = 0; i < model.visible.size(); ++i) {
        model.view->StateAt(i).visible = model.visible[i];
    }
    model.players.Apply(*model.view);
}

void DemoUi::Update(Game& game, float dt, bool paused) {
    static const uint64_t kCreate = StrCode64("DemoUiFunctor_Create");
    static const uint64_t kStart = StrCode64("DemoUiFunctor_Start");
    for (const DemoUiEvent& e : game.Demos().TakeUiEvents()) {
        const uint64_t functor = e.functor & kStrCode64Mask;
        if (functor == kCreate) {
            Create(e.demo_id, e.graph, e.file_path);
        } else if (functor == kStart) {
            Start(e.graph);
        } else if (functor == kTextFunctor) {
            Text(e.graph, e.text, game.Config().bug_screen);
        }
    }
    const size_t before = instances_.size();
    std::erase_if(instances_, [&](const auto& i) {
        return !i->demo_id.empty() && (!game.Demos().IsPlaying(i->demo_id) || game.Demos().IsHeld(i->demo_id));
    });
    if (instances_.size() != before) {
        LogInfo("ui: {} demo graphs closed with the demo", before - instances_.size());
    }
    const float step = paused ? 0.0f : dt * game.Demos().time_scale;
    for (auto& instance : instances_) {
        for (Layout& layout : instance->layouts) {
            for (ModelView& model : layout.models) {
                model.players.Update(step);
                Apply(model);
            }
        }
    }
}

void DemoUi::Draw(ui::UiBatch& batch, const UiCanvas& canvas, int language) {
    for (auto& instance : instances_) {
        if (!instance->started) {
            continue;
        }
        for (Layout& layout : instance->layouts) {
            if (!layout.visible) {
                continue;
            }
            for (ModelView& model : layout.models) {
                model.view->Draw(batch, canvas, language, 1.0f);
            }
        }
    }
}

}
