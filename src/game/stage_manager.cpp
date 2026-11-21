#include "game/stage_manager.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

#include "engine/assets/fmdl.h"
#include "engine/assets/gltf_model.h"
#include "engine/core/log.h"
#include "engine/core/parallel_for.h"
#include "engine/fs/mods.h"

namespace pt::game {
namespace {

std::string DataPackagePath(std::string fpk_path) {
    if (fpk_path.ends_with(".fpk")) {
        fpk_path.push_back('d');
    }
    return fpk_path;
}

glm::mat4 FacingTurn() {
    return glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
}

}

bool Stage::ConnectorWorld(const std::string& name, glm::mat4& out) const {
    if (!main) {
        return false;
    }
    auto it = main->connectors.find(name);
    if (it == main->connectors.end()) {
        return false;
    }
    out = ToWorld(it->second);
    return true;
}

const StageData* Stage::FileOf(const fox2::Entity* entity) const {
    for (const auto& file : files) {
        const auto& entities = file->file->Entities();
        if (!entities.empty() && entity >= entities.data() && entity < entities.data() + entities.size()) {
            return file.get();
        }
    }
    return nullptr;
}

BodyState& Stage::Body(const fox2::Entity* entity) {
    auto it = bodies_.find(entity);
    if (it != bodies_.end()) {
        return it->second;
    }
    BodyState state;
    if (const StageData* file = FileOf(entity)) {
        state.enable = file->file->GetBool(*entity, "enable", 0, true);
        if (entity->class_name == "StaticModel") {
            state.visible = (file->file->GetUInt(*entity, "flags", 0, 7) & 1) != 0;
            state.geom_active = state.visible;
        } else if (entity->class_name == "FxLocatorData") {
            state.visible = (file->file->GetUInt(*entity, "flags", 0, 7) & 1) != 0;
        }
    }
    return bodies_.emplace(entity, state).first->second;
}

const BodyState* Stage::FindBody(const fox2::Entity* entity) const {
    auto it = bodies_.find(entity);
    return it == bodies_.end() ? nullptr : &it->second;
}

namespace {
glm::mat4 TurnScale(const std::array<float, 3>& turn, const std::array<float, 3>& scale) {
    glm::mat4 m(1.0f);
    m = glm::rotate(m, glm::radians(turn[1]), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(turn[0]), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(turn[2]), glm::vec3(0, 0, 1));
    return glm::scale(m, glm::vec3(scale[0], scale[1], scale[2]));
}
}

void StageManager::ApplyModLevel(Stage& stage, std::vector<std::string>& notes) {
    const mods::ModSet* set = mods::Active();
    if (!set || (set->level_edits.empty() && set->level_adds.empty())) return;
    for (const mods::LevelEdit& edit : set->level_edits) {
        if (!mods::LevelStageMatches(edit.stage, stage.package_path)) continue;
        int matched = 0;
        for (const auto& file : stage.files) {
            auto& models = file->static_models;
            for (size_t i = 0; !edit.model.empty() && i < models.size();) {
                StaticModelPlacement& m = models[i];
                if (!mods::LevelNameMatches(edit.model, m.name)) {
                    ++i;
                    continue;
                }
                ++matched;
                if (edit.hide) {
                    models.erase(models.begin() + static_cast<ptrdiff_t>(i));
                    continue;
                }
                m.world = glm::translate(glm::mat4(1.0f), glm::vec3(edit.move[0], edit.move[1], edit.move[2])) * m.world *
                          TurnScale(edit.turn, edit.scale);
                if (!edit.collision) m.geom_file.clear();
                if (!edit.replace.empty()) m.model_file = edit.replace;
                ++i;
            }
            for (LightPlacement& l : file->lights) {
                if (edit.light.empty() || !mods::LevelNameMatches(edit.light, l.name)) continue;
                ++matched;
                l.mod_off = l.mod_off || edit.off;
                l.mod_tint *= glm::vec3(edit.tint[0], edit.tint[1], edit.tint[2]);
                l.mod_brightness *= edit.brightness;
            }
        }
        notes.push_back(std::format("mods: {} level.json: {} {} in {}: {} matched{}", edit.mod, edit.model.empty() ? "light" : "model",
                                    edit.model.empty() ? edit.light : edit.model, stage.package_path, matched,
                                    matched ? "" : " (check the entity name and the stage)"));
    }
    for (const mods::LevelAdd& add : set->level_adds) {
        if (!mods::LevelStageMatches(add.stage, stage.package_path)) continue;
        StaticModelPlacement m;
        m.name = add.name;
        m.model_file = add.model;
        m.world = glm::translate(glm::mat4(1.0f), glm::vec3(add.at[0], add.at[1], add.at[2])) * TurnScale(add.turn, add.scale);
        m.mod_collision = add.collision;
        stage.main->static_models.push_back(std::move(m));
        notes.push_back(std::format("mods: {} level.json: {} added to {} at ({:.2f} {:.2f} {:.2f})", add.mod, add.model, stage.package_path,
                                    add.at[0], add.at[1], add.at[2]));
    }
}

StageManager::ParsedStage StageManager::Parse(Vfs& vfs, const std::string& fpk_path, const KnownAssets* known) {
    ParsedStage parsed;
    auto stage = std::make_unique<Stage>();
    stage->package_path = fpk_path;
    stage->fpk = vfs.LoadPackage(fpk_path);
    stage->fpkd = vfs.LoadPackage(DataPackagePath(fpk_path));
    if (!stage->fpk || !stage->fpkd) {
        parsed.errors.push_back(std::format("stage: cannot load {}", fpk_path));
        return parsed;
    }
    for (const auto& entry : stage->fpkd->Entries()) {
        if (entry.path.ends_with(".lua")) {
            stage->scripts.push_back({entry.path, stage->fpkd->Read(entry)});
            continue;
        }
        if (!entry.path.ends_with(".fox2")) {
            continue;
        }
        auto file = std::make_shared<fox2::DataSetFile>();
        const auto bytes = stage->fpkd->Read(entry);
        if (!file->Load(entry.path, bytes)) {
            parsed.errors.push_back(std::format("stage: {} has a bad fox2 {}", fpk_path, entry.path));
            continue;
        }
        stage->files.push_back(BuildStageData(file, fpk_path));
        if (!stage->main && stage->files.back()->has_root) {
            stage->main = stage->files.back().get();
        }
    }
    if (stage->files.empty()) {
        parsed.errors.push_back(std::format("stage: {} has no fox2", fpk_path));
        return parsed;
    }
    if (!stage->main) {
        stage->main = stage->files.front().get();
    }
    ApplyModLevel(*stage, parsed.notes);
    stage->file_root = stage->main->has_root ? stage->main->root : glm::mat4(1.0f);
    if (known) {
        std::unordered_set<std::string> stems;
        for (const auto& file : stage->files) {
            for (const auto& placement : file->static_models) {
                if (placement.model_file.empty() || known->models.contains(placement.model_file) || parsed.models.contains(placement.model_file)) {
                    continue;
                }
                auto model = std::make_shared<FmdlModel>();
                if (!LoadModelFile(vfs, placement.model_file, *model)) {
                    model.reset();
                }
                if (model) {
                    for (const FmdlMaterial& m : model->materials) {
                        for (const auto& [slot, path] : m.textures) {
                            std::string stem = FtexStem(path);
                            if (!path.empty() && !known->textures.contains(stem) && stems.insert(stem).second) {
                                auto ftex = std::make_shared<FtexTexture>();
                                if (LoadFtex(vfs.Textures(), stem, *ftex)) {
                                    parsed.textures.emplace_back(path, std::move(ftex));
                                }
                            }
                        }
                    }
                }
                parsed.models.emplace(placement.model_file, std::move(model));
            }
        }
        std::unordered_map<std::string, std::shared_ptr<FmdlModel>> position_models;
        for (const auto& file : stage->files) {
            for (const auto& placement : file->static_models) {
                if (placement.geom_file.empty() || parsed.geoms.contains(placement.geom_file)) {
                    continue;
                }
                auto geom = std::make_shared<PreGeom>();
                if (auto bytes = vfs.ReadFile(placement.geom_file)) {
                    geom->bytes = std::move(*bytes);
                    if (!LoadGeom(geom->bytes, {}, geom->hull, &geom->skipped)) {
                        geom.reset();
                    }
                } else {
                    geom.reset();
                }
                if (geom && !known->geoms.contains(placement.geom_file)) {
                    const FmdlModel* model = nullptr;
                    if (auto it = parsed.models.find(placement.model_file); it != parsed.models.end()) {
                        model = it->second.get();
                    } else if (!placement.model_file.empty()) {
                        auto& slot = position_models[placement.model_file];
                        if (!slot) {
                            slot = std::make_shared<FmdlModel>();
                            if (!LoadModelFile(vfs, placement.model_file, *slot)) {
                                slot.reset();
                            }
                        }
                        model = slot.get();
                    }
                    geom->surfaces = BuildSurfaces(vfs, placement.model_file, geom->bytes, geom->hull, geom->skipped, model);
                    geom->lines = BuildLines(geom->bytes, model ? std::span<const glm::vec3>(model->raw_positions) : std::span<const glm::vec3>());
                    geom->derived = true;
                }
                parsed.geoms.emplace(placement.geom_file, std::move(geom));
            }
        }
    }
    parsed.stage = std::move(stage);
    return parsed;
}

void StageManager::StartPrefetch(const std::string& fpk_path) {
    DropPrefetch();
    prefetch_path_ = fpk_path;
    auto known = std::make_shared<KnownAssets>(KnownAssets{models_.Paths(), models_.Textures().Names(), {}});
    for (const auto& [geom_file, lines] : line_cache_) {
        known->geoms.insert(geom_file);
    }
    prefetch_ = std::make_unique<Prefetch>();
    prefetch_->task = std::make_unique<ParallelTask>([&vfs = vfs_, fpk_path, known, out = &prefetch_->parsed] {
        *out = Parse(vfs, fpk_path, known.get());
    });
}

void StageManager::DropPrefetch() {
    prefetch_.reset();
    prefetch_path_.clear();
}

void StageManager::PrefetchMazeFor(std::string_view floor) {
    if (!prefetch_allowed_) {
        return;
    }
    for (const auto& [label, stage] : stages_) {
        for (const auto& data : stage->files) {
            const fox2::DataSetFile& f = *data->file;
            for (const fox2::Entity& e : f.Entities()) {
                if (!e.FindDynamic("nextMazeFloor") || f.GetString(e, "nextMazeFloor") != floor) {
                    continue;
                }
                const std::string maze = f.GetString(e, "targetMazeFpk");
                const bool loaded = std::any_of(stages_.begin(), stages_.end(), [&](const auto& s) { return s.second->package_path == maze; });
                if (!maze.empty() && maze != prefetch_path_ && !loaded) {
                    LogDebug("stage: floor {} loads {} next, parsed ahead", floor, maze);
                    deferred_prefetch_.clear();
                    StartPrefetch(maze);
                }
                return;
            }
        }
    }
}

std::unique_ptr<Stage> StageManager::Load(const std::string& fpk_path, const std::string& label) {
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::time_point from, clock::time_point to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
    const auto started = clock::now();
    ++load_count_;
    ParsedStage parsed;
    const bool prefetched = prefetch_ && prefetch_path_ == fpk_path;
    if (prefetched) {
        prefetch_->task->Wait();
        parsed = std::move(prefetch_->parsed);
        prefetch_.reset();
        prefetch_path_.clear();
    } else {
        DropPrefetch();
        parsed = Parse(vfs_, fpk_path);
    }
    const auto parsed_at = clock::now();
    for (const std::string& error : parsed.errors) {
        LogError("{}", error);
    }
    for (const std::string& note : parsed.notes) {
        LogInfo("{}", note);
    }
    if (!parsed.stage) {
        return nullptr;
    }
    auto stage = std::move(parsed.stage);
    for (const auto& file : stage->files) {
        LogStageData(*file);
    }
    stage->id = next_id_++;
    stage->label = label;
    for (const auto& data : stage->files) {
        const fox2::DataSetFile& f = *data->file;
        for (const fox2::Entity& e : f.Entities()) {
            if (!e.FindDynamic("targetStageLabel") || !e.FindDynamic("hijackerStageLabel")) {
                continue;
            }
            std::pair<std::string, std::string> swap{f.GetString(e, "targetStageLabel"), f.GetString(e, "hijackerStageLabel")};
            if (!swap.first.empty() && !swap.second.empty() && swap.first != swap.second &&
                std::find(stage->swaps.begin(), stage->swaps.end(), swap) == stage->swaps.end()) {
                stage->swaps.push_back(std::move(swap));
            }
        }
    }
    for (const auto& file : stage->files) {
        for (const auto& placement : file->static_models) {
            stage->Body(placement.entity);
        }
    }
    std::vector<std::string> adopted;
    for (auto& [path, ftex] : parsed.textures) {
        if (models_.Textures().AdoptDecoded(path, std::move(ftex))) {
            adopted.push_back(path);
        }
    }
    const uint32_t models_before = models_.Loaded() + models_.Failed();
    const double read_before = models_.Textures().ReadMs();
    const double upload_before = models_.Textures().UploadMs();
    double models_ms = 0.0;
    double geoms_ms = 0.0;
    const ModelCache::StageScope stage_models(models_);
    std::unordered_set<std::string> referenced;
    for (const auto& file : stage->files) {
        for (const auto& placement : file->static_models) {
            const auto model_started = clock::now();
            const auto pre_model = parsed.models.find(placement.model_file);
            const ModelEntry* model = models_.Get(placement.model_file, pre_model != parsed.models.end() ? pre_model->second.get() : nullptr);
            if (referenced.insert(placement.model_file).second) {
                models_.AddStageRef(placement.model_file);
                stage->model_refs.push_back(placement.model_file);
            }
            const auto model_done = clock::now();
            models_ms += ms(model_started, model_done);
            if (model) {
                stage->draws.push_back({file.get(), placement.entity, model->mesh.get(), placement.world, placement.color});
            }
            if (model && (placement.mod_collision == "box" || placement.mod_collision == "mesh")) {
                Stage::CollisionPiece piece;
                piece.file_transform = placement.world;
                piece.model_file = placement.model_file;
                auto triangle = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
                    GeomTriangle t{};
                    t.a = a;
                    t.b = b;
                    t.c = c;
                    t.shape_flags = 0x200;
                    piece.triangles.push_back(t);
                };
                MeshData mesh;
                if (placement.mod_collision == "mesh" && models_.ReadMesh(placement.model_file, mesh)) {
                    for (const SubMesh& sub : mesh.submeshes) {
                        for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
                            const uint32_t* k = mesh.indices.data() + sub.first_index + i;
                            auto at = [&](uint32_t j) { return mesh.vertices[static_cast<size_t>(sub.vertex_offset + static_cast<int32_t>(j))].position; };
                            triangle(at(k[0]), at(k[1]), at(k[2]));
                        }
                    }
                } else {
                    const glm::vec3 lo = model->mesh->bounds_min, hi = model->mesh->bounds_max;
                    const glm::vec3 p[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                                            {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
                    const int faces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {3, 2, 6, 7}, {0, 3, 7, 4}, {1, 5, 6, 2}};
                    for (const auto& f : faces) {
                        triangle(p[f[0]], p[f[1]], p[f[2]]);
                        triangle(p[f[0]], p[f[2]], p[f[3]]);
                    }
                }
                if (!piece.triangles.empty()) stage->collision.push_back(std::move(piece));
            }
            if (!placement.geom_file.empty()) {
                Stage::CollisionPiece piece;
                piece.entity = placement.entity;
                piece.file_transform = placement.world;
                piece.geom_file = placement.geom_file;
                piece.model_file = placement.model_file;
                if (const auto pre = parsed.geoms.find(placement.geom_file); pre != parsed.geoms.end()) {
                    if (pre->second) {
                        if (pre->second->derived) {
                            surface_cache_.try_emplace(placement.geom_file, pre->second->surfaces);
                            line_cache_.try_emplace(placement.geom_file, pre->second->lines);
                        }
                        piece.triangles = pre->second->hull;
                        piece.surfaces = Surfaces(placement.geom_file, placement.model_file, pre->second->bytes, piece.triangles, pre->second->skipped);
                        piece.lines = LineChecks(placement.geom_file, pre->second->bytes, model);
                        stage->collision.push_back(std::move(piece));
                    }
                } else if (auto bytes = vfs_.ReadFile(placement.geom_file)) {
                    bool skipped = false;
                    if (LoadGeom(*bytes, {}, piece.triangles, &skipped)) {
                        piece.surfaces = Surfaces(placement.geom_file, placement.model_file, *bytes, piece.triangles, skipped);
                        piece.lines = LineChecks(placement.geom_file, *bytes, model);
                        stage->collision.push_back(std::move(piece));
                    }
                }
                geoms_ms += ms(model_done, clock::now());
            }
        }
    }
    models_.Textures().DropDecoded(adopted);
    const double total_ms = ms(started, clock::now());
    if (total_ms > 5.0) {
        LogInfo("stage: {} loaded in {:.1f} ms main thread (parse {:.1f}{}, models {:.1f}, {} new: tex read {:.1f}, tex upload {:.1f}; "
                "geoms {:.1f})", fpk_path, total_ms, ms(started, parsed_at), prefetched ? " taken from the prefetch" : "",
                models_ms, models_.Loaded() + models_.Failed() - models_before, models_.Textures().ReadMs() - read_before,
                models_.Textures().UploadMs() - upload_before, geoms_ms);
    }
    LogDebug("stage: {} textures of {}, {} materials of {} in use", models_.Textures().TextureCount(), TextureManager::kMaxTextures,
             models_.Textures().MaterialCount(), TextureManager::kMaxMaterials);
    return stage;
}

void StageManager::Place(Stage& stage, const glm::mat4& runtime_root) {
    DropCollisionBuild();
    stage.runtime_root = runtime_root;
    stage.file_to_world = runtime_root * glm::inverse(stage.file_root);
    MarkVisualsDirty();
    MarkCollisionDirty();
}

Stage* StageManager::LoadResident(const std::string& fpk_path) {
    auto stage = Load(fpk_path, "_resident_");
    if (!stage) {
        return nullptr;
    }
    stage->resident = true;
    stage->active = true;
    Place(*stage, stage->file_root);
    resident_ = std::move(stage);
    if (on_loaded) {
        on_loaded(*resident_);
    }
    LogInfo("stage: resident {} loaded ({} files, {} scripts)", fpk_path, resident_->files.size(), resident_->scripts.size());
    return resident_.get();
}

void StageManager::RequestLocation(const std::string& fpk_path) {
    pending_location_ = fpk_path;
}

void StageManager::RequestLoad(const std::string& fpk_path, const std::string& label, const std::string& connector, const std::string& base_label,
                               const std::string& base_connector) {
    if (stages_.contains(label)) {
        LogWarn("stage: LoadStage({}) ignored, label {} exists", fpk_path, label);
        return;
    }
    pending_load_ = PendingLoad{fpk_path, label, connector, base_label, base_connector};
}

void StageManager::RequestActivate(const std::string& label) {
    pending_activate_ = label;
}

void StageManager::RequestUnload(const std::string& label) {
    pending_unload_ = label;
}

void StageManager::Update() {
    if (!deferred_prefetch_.empty() && !CollisionBuildRunning()) {
        const std::string next = std::move(deferred_prefetch_);
        deferred_prefetch_.clear();
        if (prefetch_allowed_) {
            StartPrefetch(next);
        }
    }
    if (pending_location_) {
        const std::string path = *pending_location_;
        pending_location_.reset();
        UnloadAll();
        SetNextStageByPath(path);
        return;
    }
    if (pending_load_) {
        const PendingLoad load = *pending_load_;
        pending_load_.reset();
        if (stages_.contains(load.label) || stages_.size() >= kSlots) {
            LogWarn("stage: load of {} as {} dropped ({} slots used)", load.fpk_path, load.label, stages_.size());
            return;
        }
        LoadStage(load.fpk_path, load.label, load.connector, load.base_label, load.base_connector);
        return;
    }
    if (pending_activate_) {
        const std::string label = *pending_activate_;
        pending_activate_.reset();
        ActivateStage(label);
        return;
    }
    if (pending_unload_) {
        const std::string label = *pending_unload_;
        pending_unload_.reset();
        UnloadStage(label);
    }
}

bool StageManager::IsActive(const std::string& label) const {
    auto it = stages_.find(label);
    return it != stages_.end() && it->second->active;
}

bool StageManager::IsLoadedInactive(const std::string& label) const {
    auto it = stages_.find(label);
    return it != stages_.end() && !it->second->active;
}

Stage* StageManager::SetNextStageByPath(const std::string& fpk_path) {
    auto stage = Load(fpk_path, "current");
    if (!stage) {
        return nullptr;
    }
    Place(*stage, stage->file_root);
    stage->active = true;
    if (auto it = stages_.find("current"); it != stages_.end()) {
        auto old = std::move(it->second);
        stages_.erase(it);
        Unload(std::move(old));
    }
    Stage* raw = stage.get();
    stages_["current"] = std::move(stage);
    if (on_loaded) {
        on_loaded(*raw);
    }
    LogInfo("stage: {} set as current (id {})", fpk_path, raw->id);
    return raw;
}

Stage* StageManager::LoadStage(const std::string& fpk_path, const std::string& label, const std::string& connector,
                               const std::string& base_label, const std::string& base_connector) {
    Stage* base = Find(base_label);
    glm::mat4 base_world(1.0f);
    if (!base || !base->ConnectorWorld(base_connector, base_world)) {
        LogError("stage: base {}:{} not found for {}", base_label, base_connector, fpk_path);
        return nullptr;
    }
    auto stage = Load(fpk_path, label);
    if (!stage) {
        return nullptr;
    }
    auto it = stage->main->connectors.find(connector);
    if (it == stage->main->connectors.end()) {
        LogError("stage: {} has no connector {}", fpk_path, connector);
        return nullptr;
    }
    const glm::mat4 local = glm::inverse(stage->file_root) * it->second;
    const bool clean = collision_generation_ == built_generation_ && !stages_.contains(label);
    Place(*stage, base_world * FacingTurn() * glm::inverse(local));
    if (auto old = stages_.find(label); old != stages_.end()) {
        auto previous = std::move(old->second);
        stages_.erase(old);
        Unload(std::move(previous));
    }
    Stage* raw = stage.get();
    stages_[label] = std::move(stage);
    if (on_loaded) {
        on_loaded(*raw);
    }
    collision_deferrable_ = clean && !raw->active;
    if (prefetch_allowed_) {
        std::string next = fpk_path;
        for (const auto& data : raw->files) {
            for (const fox2::Entity& e : data->file->Entities()) {
                if (e.FindDynamic("nextMazeFloor") && e.FindDynamic("targetFpk")) {
                    if (std::string target = data->file->GetString(e, "targetFpk"); !target.empty()) {
                        next = std::move(target);
                    }
                }
            }
        }
        deferred_prefetch_ = std::move(next);
    }
    LogInfo("stage: {} loaded as {} (id {}) at {}:{} (connector {})", fpk_path, label, raw->id, base_label, base_connector, connector);
    return raw;
}

bool StageManager::ActivateStage(const std::string& label) {
    Stage* stage = Find(label);
    if (!stage) {
        LogWarn("stage: ActivateStage({}) without such stage", label);
        return false;
    }
    stage->active = true;
    MarkVisualsDirty();
    LogInfo("stage: {} (id {}) activated", label, stage->id);
    StartSwapBuild();
    return true;
}

bool StageManager::DeactivateStage(const std::string& label) {
    Stage* stage = Find(label);
    if (!stage) {
        return false;
    }
    stage->active = false;
    MarkVisualsDirty();
    return true;
}

bool StageManager::ChangeStageId(const std::string& from, const std::string& to) {
    if (CollisionBuildRunning()) {
        DropCollisionBuild();
    }
    auto it = stages_.find(from);
    if (it == stages_.end()) {
        LogWarn("stage: ChangeStageId({}, {}) without such stage", from, to);
        return false;
    }
    if (stages_.contains(to)) {
        LogWarn("stage: ChangeStageId({}, {}) refused, label in use", from, to);
        return false;
    }
    auto stage = std::move(it->second);
    stages_.erase(it);
    stage->label = to;
    stages_[to] = std::move(stage);
    MarkVisualsDirty();
    MarkCollisionDirty();
    LogInfo("stage: {} renamed to {}", from, to);
    return true;
}

void StageManager::Unload(std::unique_ptr<Stage> stage) {
    if (!stage) {
        return;
    }
    DropCollisionBuildReading(stage.get());
    if (on_unloading) {
        on_unloading(*stage);
    }
    LogInfo("stage: {} (id {}, {}) unloaded", stage->label, stage->id, stage->package_path);
    for (const std::string& path : stage->model_refs) {
        models_.ReleaseStageRef(path);
    }
    stage.reset();
    MarkVisualsDirty();
    MarkCollisionDirty();
}

bool StageManager::UnloadStage(const std::string& label) {
    auto it = stages_.find(label);
    if (it == stages_.end()) {
        return false;
    }
    auto stage = std::move(it->second);
    stages_.erase(it);
    Unload(std::move(stage));
    return true;
}

void StageManager::UnloadAll() {
    while (!stages_.empty()) {
        auto it = stages_.begin();
        auto stage = std::move(it->second);
        stages_.erase(it);
        Unload(std::move(stage));
    }
}

Stage* StageManager::Find(const std::string& label) {
    auto it = stages_.find(label);
    return it == stages_.end() ? nullptr : it->second.get();
}

Stage* StageManager::FindById(uint32_t id) {
    if (resident_ && resident_->id == id) {
        return resident_.get();
    }
    for (auto& [label, stage] : stages_) {
        if (stage->id == id) {
            return stage.get();
        }
    }
    return nullptr;
}

void StageManager::ForEachStage(const std::function<void(Stage&)>& fn, bool include_resident) {
    if (include_resident && resident_) {
        fn(*resident_);
    }
    std::vector<Stage*> list;
    for (auto& [label, stage] : stages_) {
        list.push_back(stage.get());
    }
    for (Stage* stage : list) {
        fn(*stage);
    }
}

void StageManager::CollectDraws(std::vector<DrawItem>& out) const {
    for (const auto& [label, stage] : stages_) {
        if (!stage->active) {
            continue;
        }
        for (const Stage::Draw& draw : stage->draws) {
            const BodyState* body = stage->FindBody(draw.entity);
            if (body && !body->visible) {
                continue;
            }
            DrawItem item;
            item.mesh = draw.mesh;
            item.transform = stage->file_to_world * draw.file_transform;
            item.source = (static_cast<uint64_t>(stage->id) << 48) ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&draw));
            out.push_back(item);
        }
    }
}

void StageManager::CollectLights(std::vector<std::pair<const Stage*, const LightPlacement*>>& out) const {
    for (const auto& [label, stage] : stages_) {
        for (const auto& file : stage->files) {
            for (const LightPlacement& light : file->lights) {
                const BodyState* body = stage->FindBody(light.entity);
                if (body ? body->enable : light.enable) {
                    out.emplace_back(stage.get(), &light);
                }
            }
        }
    }
}

std::shared_ptr<const std::vector<GeomTriangle>> StageManager::Surfaces(const std::string& geom_file, const std::string& model_file,
                                                                        std::span<const uint8_t> geom, const std::vector<GeomTriangle>& hull,
                                                                        bool skipped_fmdl_materials) {
    if (auto it = surface_cache_.find(geom_file); it != surface_cache_.end()) {
        return it->second;
    }
    auto result = BuildSurfaces(vfs_, model_file, geom, hull, skipped_fmdl_materials, nullptr);
    surface_cache_[geom_file] = result;
    return result;
}

std::shared_ptr<const std::vector<GeomTriangle>> StageManager::BuildSurfaces(Vfs& vfs, const std::string& model_file, std::span<const uint8_t> geom,
                                                                             const std::vector<GeomTriangle>& hull, bool skipped_fmdl_materials,
                                                                             const FmdlModel* model) {
    auto surfaces = std::make_shared<std::vector<GeomTriangle>>();
    const std::vector<GeomTriangle>* source = &hull;
    std::vector<GeomTriangle> full;
    if (skipped_fmdl_materials && !model_file.empty()) {
        FmdlModel read;
        const FmdlModel* parsed = model;
        if (!parsed) {
            if (LoadModelFile(vfs, model_file, read)) {
                parsed = &read;
            }
        }
        if (parsed && LoadGeom(geom, parsed->raw_positions, full)) {
            source = &full;
        }
    }
    for (const GeomTriangle& t : *source) {
        if (t.material != 0) {
            surfaces->push_back(t);
        }
    }
    return surfaces->empty() ? nullptr : std::shared_ptr<const std::vector<GeomTriangle>>(std::move(surfaces));
}

std::shared_ptr<const std::vector<GeomTriangle>> StageManager::LineChecks(const std::string& geom_file, std::span<const uint8_t> geom, const ModelEntry* model) {
    if (auto it = line_cache_.find(geom_file); it != line_cache_.end()) {
        return it->second;
    }
    auto result = BuildLines(geom, model ? std::span<const glm::vec3>(model->raw_positions) : std::span<const glm::vec3>());
    line_cache_[geom_file] = result;
    return result;
}

std::shared_ptr<const std::vector<GeomTriangle>> StageManager::BuildLines(std::span<const uint8_t> geom, std::span<const glm::vec3> positions) {
    std::vector<GeomTriangle> all;
    auto lines = std::make_shared<std::vector<GeomTriangle>>();
    if (LoadGeom(geom, positions, all)) {
        for (const GeomTriangle& t : all) {
            if ((t.tags & 0x700u) != 0 || (t.tags & 0x90u) == 0x90u) {
                lines->push_back(t);
            }
        }
    }
    return lines->empty() ? nullptr : std::shared_ptr<const std::vector<GeomTriangle>>(std::move(lines));
}

void StageManager::PrepareWalkSurfaces(Stage& stage, std::vector<GeomTriangle> bounds) {
    DropCollisionBuild();
    size_t pieces = 0;
    size_t triangles = 0;
    for (Stage::CollisionPiece& piece : stage.collision) {
        const StageData* file = piece.entity ? stage.FileOf(piece.entity) : nullptr;
        const std::string_view entity = piece.entity && file ? file->file->EntityName(*piece.entity) : std::string_view();
        if (!piece.entity || piece.walk || entity.starts_with("shsb_leaf") || entity == "shsb_carr001_0002") {
            continue;
        }
        auto it = walk_cache_.find(piece.geom_file);
        if (it == walk_cache_.end()) {
            auto full = std::make_shared<std::vector<GeomTriangle>>();
            const ModelCache::StageScope stage_models(models_);
            const ModelEntry* model = models_.Get(piece.model_file);
            if (std::find(stage.model_refs.begin(), stage.model_refs.end(), piece.model_file) == stage.model_refs.end()) {
                models_.AddStageRef(piece.model_file);
                stage.model_refs.push_back(piece.model_file);
            }
            if (auto bytes = vfs_.ReadFile(piece.geom_file)) {
                LoadGeom(*bytes, model ? std::span<const glm::vec3>(model->raw_positions) : std::span<const glm::vec3>(), *full);
            }
            it = walk_cache_.emplace(piece.geom_file, std::move(full)).first;
        }
        piece.walk = it->second;
        piece.cache.valid = false;
        ++pieces;
        triangles += piece.walk->size();
    }
    std::erase_if(stage.collision, [](const Stage::CollisionPiece& piece) { return !piece.entity; });
    Stage::CollisionPiece walls;
    walls.triangles = std::move(bounds);
    auto lines = std::make_shared<std::vector<GeomTriangle>>(walls.triangles);
    for (GeomTriangle& t : *lines) t.tags |= 0x100u;
    walls.lines = std::move(lines);
    stage.collision.push_back(std::move(walls));
    stage.walk_detail_surfaces = true;
    MarkCollisionDirty();
    LogInfo("stage: {} walk surfaces of {} pieces, {} triangles", stage.label, pieces, triangles);
}

struct StageManager::PendingCollision {
    enum class Kind { Preload, Swap };
    Kind kind = Kind::Preload;
    std::vector<CollisionEntry> entries;
    std::vector<glm::mat4> transforms;
    std::vector<const std::vector<GeomTriangle>*> walks;
    std::unordered_set<const Stage*> reads;
    std::string swap;
    CollisionWorld world;
    CollisionWorld lines;
    CollisionWorld reflections;
    std::vector<CollisionOwners> owners;
    double milliseconds = 0.0;
    std::unique_ptr<ParallelTask> task;

    static const std::vector<GeomTriangle>* Walk(const CollisionEntry& entry) {
        return entry.stage->walk_detail_surfaces ? entry.piece->walk.get() : nullptr;
    }
    void Start(std::vector<CollisionEntry> from) {
        entries = std::move(from);
        for (const CollisionEntry& entry : entries) {
            transforms.push_back(entry.stage->file_to_world * entry.piece->file_transform);
            walks.push_back(Walk(entry));
            reads.insert(entry.stage);
        }
        task = std::make_unique<ParallelTask>([this] {
            const auto start = std::chrono::steady_clock::now();
            BuildWorlds(entries, world, &lines, &reflections, owners);
            milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        });
    }
    bool BuiltFrom(const std::vector<CollisionEntry>& now) const {
        if (now.size() != entries.size()) {
            return false;
        }
        for (size_t i = 0; i < now.size(); ++i) {
            if (now[i].stage != entries[i].stage || now[i].piece != entries[i].piece || now[i].label != entries[i].label ||
                now[i].stage->file_to_world * now[i].piece->file_transform != transforms[i] || Walk(now[i]) != walks[i]) {
                return false;
            }
        }
        return true;
    }
};

StageManager::StageManager(Vfs& vfs, ModelCache& models) : vfs_(vfs), models_(models) {}

StageManager::~StageManager() {
    DropCollisionBuild();
    DropPrefetch();
}

void StageManager::DropCollisionBuild() {
    pending_collision_.reset();
}

void StageManager::DropCollisionBuildReading(const Stage* stage) {
    if (pending_collision_ && pending_collision_->reads.contains(stage)) {
        DropCollisionBuild();
    }
}

bool StageManager::CollisionBuildRunning() const {
    return pending_collision_ && pending_collision_->kind == PendingCollision::Kind::Preload;
}

void StageManager::StartSwapBuild() {
    if (pending_collision_ || collision_generation_ != built_generation_) {
        return;
    }
    for (const auto& [owner_label, owner] : stages_) {
        for (const auto& [target, hijacker] : owner->swaps) {
            const auto t = stages_.find(target);
            const auto h = stages_.find(hijacker);
            if (t == stages_.end() || h == stages_.end() || !h->second->active) {
                continue;
            }
            std::map<std::string, const Stage*> after;
            for (const auto& [label, stage] : stages_) {
                if (label != target && label != hijacker) {
                    after[label] = stage.get();
                }
            }
            after[target] = h->second.get();
            std::vector<CollisionEntry> entries;
            for (const auto& [label, stage] : after) {
                for (const Stage::CollisionPiece& piece : stage->collision) {
                    entries.push_back({stage, label, &piece});
                }
            }
            pending_collision_ = std::make_unique<PendingCollision>();
            pending_collision_->kind = PendingCollision::Kind::Swap;
            pending_collision_->swap = std::format("{} into {}", hijacker, target);
            pending_collision_->Start(std::move(entries));
            LogDebug("stage: collision after the swap of {} built on a worker", pending_collision_->swap);
            return;
        }
    }
}

std::vector<StageManager::CollisionEntry> StageManager::CollisionEntries() const {
    std::vector<CollisionEntry> entries;
    for (const auto& [label, stage] : stages_) {
        for (const Stage::CollisionPiece& piece : stage->collision) {
            entries.push_back({stage.get(), label, &piece});
        }
    }
    return entries;
}

void StageManager::BuildWorlds(const std::vector<CollisionEntry>& entries, CollisionWorld& world, CollisionWorld* lines,
                               CollisionWorld* reflections, std::vector<CollisionOwners>& owners) {
    world.Clear();
    if (lines) {
        lines->Clear();
    }
    if (reflections) {
        reflections->Clear();
    }
    size_t triangles = 0;
    for (const CollisionEntry& entry : entries) {
        triangles += entry.piece->triangles.size() + (entry.piece->lines ? entry.piece->lines->size() : 0);
    }
    ParallelChunks(entries.size(), ParallelChunkCount(triangles, 32768), [&](size_t, size_t begin, size_t end) {
        for (size_t e = begin; e < end; ++e) {
            const Stage::CollisionPiece& piece = *entries[e].piece;
            Stage::CollisionPiece::Cache& cache = piece.cache;
            const glm::mat4 transform = entries[e].stage->file_to_world * piece.file_transform;
            if (cache.valid && cache.transform == transform) {
                continue;
            }
            cache.valid = true;
            cache.transform = transform;
            cache.world.clear();
            cache.lines.clear();
            cache.reflections.clear();
            cache.surfaces.clear();
            CollisionTriangle c;
            for (const GeomTriangle& t : piece.triangles) {
                if (t.node == 0 && CollisionWorld::MakeTriangle(t, transform, c)) {
                    cache.world.push_back(c);
                }
            }
            if (piece.walk && entries[e].stage->walk_detail_surfaces) {
                for (const GeomTriangle& t : *piece.walk) {
                    if (t.node == 1 && CollisionWorld::MakeTriangle(t, transform, c)) {
                        cache.world.push_back(c);
                    }
                }
            }
            if (piece.lines) {
                for (const GeomTriangle& t : *piece.lines) {
                    if ((t.tags & 0x700u) && CollisionWorld::MakeTriangle(t, transform, c)) {
                        cache.lines.push_back(c);
                    }
                    if ((t.tags & 0x90u) == 0x90u && CollisionWorld::MakeTriangle(t, transform, c)) {
                        cache.reflections.push_back(c);
                    }
                }
            }
            if (piece.surfaces) {
                for (const GeomTriangle& t : *piece.surfaces) {
                    cache.surfaces.push_back({glm::vec3(transform * glm::vec4(t.a, 1.0f)), glm::vec3(transform * glm::vec4(t.b, 1.0f)),
                                              glm::vec3(transform * glm::vec4(t.c, 1.0f)), t.material});
                }
            }
            if (cache.owner.empty() || cache.owner_label != entries[e].label) {
                const StageData* file = piece.entity ? entries[e].stage->FileOf(piece.entity) : nullptr;
                cache.owner_label = entries[e].label;
                cache.owner = file ? entries[e].label + ":" + file->file->EntityName(*piece.entity) : entries[e].label;
            }
        }
    });
    owners.clear();
    std::vector<CollisionWorld::Block> world_blocks;
    std::vector<CollisionWorld::Block> line_blocks;
    std::vector<CollisionWorld::Block> reflection_blocks;
    for (const CollisionEntry& entry : entries) {
        const Stage::CollisionPiece& piece = *entry.piece;
        const Stage::CollisionPiece::Cache& cache = piece.cache;
        CollisionOwners owner{entry.stage, &piece, world.AddOwner(cache.owner), 0, 0};
        world_blocks.push_back({&cache.world, owner.world});
        if (lines && piece.lines) {
            owner.line = lines->AddOwner(cache.owner);
            line_blocks.push_back({&cache.lines, owner.line});
        }
        if (reflections && piece.lines) {
            owner.reflection = reflections->AddOwner(cache.owner);
            reflection_blocks.push_back({&cache.reflections, owner.reflection});
        }
        owners.push_back(owner);
    }
    world.AppendBlocks(world_blocks);
    world.Build();
    if (reflections) {
        reflections->AppendBlocks(reflection_blocks);
        reflections->Build();
    }
    if (lines) {
        lines->AppendBlocks(line_blocks);
        lines->Build();
    }
}

void StageManager::BuildCollision(CollisionWorld& world, std::vector<SurfaceTriangle>* surfaces, CollisionWorld* lines,
                                  CollisionWorld* reflections) {
    const auto start = std::chrono::steady_clock::now();
    if (pending_collision_ && pending_collision_->kind == PendingCollision::Kind::Swap && lines && reflections) {
        if (pending_collision_->BuiltFrom(CollisionEntries())) {
            pending_collision_->task->Wait();
            collision_deferrable_ = false;
            built_generation_ = collision_generation_;
            std::swap(world, pending_collision_->world);
            std::swap(*lines, pending_collision_->lines);
            std::swap(*reflections, pending_collision_->reflections);
            std::swap(collision_owners_, pending_collision_->owners);
            const double built = pending_collision_->milliseconds;
            const std::string swap = pending_collision_->swap;
            pending_collision_.reset();
            UpdateCollisionActive(world, surfaces, lines, reflections);
            LogDebug("stage: collision {} triangles, {} material surfaces, {} line check triangles (swap of {}, worker {:.1f} ms, taken in "
                     "{:.2f} ms)", world.ActiveTriangles(), surfaces ? surfaces->size() : 0, lines->ActiveTriangles(), swap, built,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            StartSwapBuild();
            return;
        }
        LogDebug("stage: collision after the swap of {} not taken, the stages changed otherwise", pending_collision_->swap);
    }
    DropCollisionBuild();
    collision_deferrable_ = false;
    built_generation_ = collision_generation_;
    BuildWorlds(CollisionEntries(), world, lines, reflections, collision_owners_);
    UpdateCollisionActive(world, surfaces, lines, reflections);
    LogDebug("stage: collision {} triangles, {} material surfaces, {} line check triangles ({:.1f} ms)", world.ActiveTriangles(),
             surfaces ? surfaces->size() : 0, lines ? lines->ActiveTriangles() : 0,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    StartSwapBuild();
}

void StageManager::StartCollisionBuild() {
    DropCollisionBuild();
    collision_deferrable_ = false;
    built_generation_ = collision_generation_;
    pending_collision_ = std::make_unique<PendingCollision>();
    pending_collision_->Start(CollisionEntries());
}

void StageManager::FinishCollisionBuild(CollisionWorld& world, std::vector<SurfaceTriangle>* surfaces, CollisionWorld& lines,
                                        CollisionWorld& reflections) {
    if (!CollisionBuildRunning()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    pending_collision_->task->Wait();
    std::swap(world, pending_collision_->world);
    std::swap(lines, pending_collision_->lines);
    std::swap(reflections, pending_collision_->reflections);
    std::swap(collision_owners_, pending_collision_->owners);
    const double built = pending_collision_->milliseconds;
    pending_collision_.reset();
    UpdateCollisionActive(world, surfaces, &lines, &reflections);
    LogDebug("stage: collision {} triangles, {} material surfaces, {} line check triangles (worker {:.1f} ms, taken in {:.2f} ms)",
             world.ActiveTriangles(), surfaces ? surfaces->size() : 0, lines.ActiveTriangles(), built,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    StartSwapBuild();
}

void StageManager::UpdateCollisionActive(CollisionWorld& world, std::vector<SurfaceTriangle>* surfaces, CollisionWorld* lines,
                                         CollisionWorld* reflections) const {
    if (surfaces) {
        surfaces->clear();
    }
    for (const CollisionOwners& owner : collision_owners_) {
        const BodyState* body = owner.stage->FindBody(owner.piece->entity);
        const bool active = !body || body->geom_active;
        world.SetOwnerActive(owner.world, active);
        if (lines && owner.piece->lines) {
            lines->SetOwnerActive(owner.line, active);
        }
        if (reflections && owner.piece->lines) {
            reflections->SetOwnerActive(owner.reflection, active);
        }
        if (active && surfaces && owner.piece->surfaces) {
            surfaces->insert(surfaces->end(), owner.piece->cache.surfaces.begin(), owner.piece->cache.surfaces.end());
        }
    }
}

}
