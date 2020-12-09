// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/visualization/gui/PickPointsInteractor.h"

#include "open3d/geometry/Image.h"
#include "open3d/geometry/PointCloud.h"
#include "open3d/geometry/TriangleMesh.h"
#include "open3d/t/geometry/PointCloud.h"
#include "open3d/t/geometry/TriangleMesh.h"
#include "open3d/utility/Console.h"
#include "open3d/visualization/gui/Events.h"
#include "open3d/visualization/rendering/Material.h"
#include "open3d/visualization/rendering/Open3DScene.h"
#include "open3d/visualization/rendering/Scene.h"
#include "open3d/visualization/rendering/View.h"

#define WANT_DEBUG_IMAGE	0

#if WANT_DEBUG_IMAGE
#include "open3d/io/ImageIO.h"
#endif  // WANT_DEBUG_IMAGE

namespace open3d {
namespace visualization {
namespace gui {

namespace {
// Background color is white, so that index 0 can be black
static const Eigen::Vector4f kBackgroundColor = {1.0f, 1.0f, 1.0f, 1.0f};
static const std::string kSelectablePointsName = "__selectable_points";
// The maximum pickable point is one less than FFFFFF, because that would
// be white, which is the color of the background.
static const unsigned int kNoIndex = 0x00ffffff;
static const unsigned int kMaxPickableIndex = 0x00fffffe;

Eigen::Vector3d SetColorForIndex(uint32_t idx) {
    idx = std::min(kMaxPickableIndex, idx);
    const double red = double((idx & 0x00ff0000) >> 16) / 255.0;
    const double green = double((idx & 0x0000ff00) >> 8) / 255.0;
    const double blue = double((idx & 0x000000ff)) / 255.0;
    return {red, green, blue};
}

uint32_t GetIndexForColor(geometry::Image *image, int x, int y) {
    uint8_t *rgb = image->PointerAt<uint8_t>(x, y, 0);
    const unsigned int red = (static_cast<unsigned int>(rgb[0]) << 16);
    const unsigned int green = (static_cast<unsigned int>(rgb[1]) << 8);
    const unsigned int blue = (static_cast<unsigned int>(rgb[2]));
    return (red | green | blue);
}

}  // namespace

// ----------------------------------------------------------------------------
class SelectionIndexLookup {
private:
    struct Obj {
        std::string name;
        size_t start_index;

        Obj(const std::string &n, size_t start) : name(n), start_index(start){};
    };

public:
    void Clear() { objects_.clear(); }

    // start_index must be larger than all previously added items
    void Add(const std::string &name, size_t start_index) {
        assert(objects_.empty() || objects_.back().start_index < start_index);
        objects_.emplace_back(name, start_index);
        assert(objects_[0].start_index == 0);
    }

    const Obj &ObjectForIndex(size_t index) {
        if (objects_.size() == 1) {
            return objects_[0];
        } else {
            auto next = std::upper_bound(objects_.begin(), objects_.end(),
                                         index, [](size_t value, const Obj &o) {
                                             return value < o.start_index;
                                         });
            assert(next != objects_.end());  // first object != 0
            if (next == objects_.end()) {
                return objects_.back();

            } else {
                --next;
                return *next;
            }
        }
    }

private:
    std::vector<Obj> objects_;
};

// ----------------------------------------------------------------------------
PickPointsInteractor::PickPointsInteractor(rendering::Open3DScene *scene,
                                           rendering::Camera *camera) {
    scene_ = scene;
    camera_ = camera;
    picking_scene_ =
            std::make_shared<rendering::Open3DScene>(scene->GetRenderer());

    picking_scene_->SetDownsampleThreshold(SIZE_MAX);  // don't downsample!
    picking_scene_->SetBackgroundColor(kBackgroundColor);

    picking_scene_->GetView()->ConfigureForColorPicking();
}

PickPointsInteractor::~PickPointsInteractor() { delete lookup_; }

void PickPointsInteractor::SetPointSize(int px) {
    point_size_ = px;
    if (!points_.empty()) {
        auto mat = MakeMaterial();
        picking_scene_->GetScene()->OverrideMaterial(kSelectablePointsName,
                                                     mat);
    }
}

void PickPointsInteractor::SetPickableGeometry(
        const std::vector<SceneWidget::PickableGeometry> &geometry) {
    delete lookup_;
    lookup_ = new SelectionIndexLookup();

    picking_scene_->ClearGeometry();
    SetNeedsRedraw();

    // Record the points (for selection), and add a depth-write copy of any
    // TriangleMesh so that occluded points are not selected.
    points_.clear();
    for (auto &pg : geometry) {
        lookup_->Add(pg.name, points_.size());

        auto cloud = dynamic_cast<const geometry::PointCloud *>(pg.geometry);
        auto tcloud =
                dynamic_cast<const t::geometry::PointCloud *>(pg.tgeometry);
        auto mesh = dynamic_cast<const geometry::TriangleMesh *>(pg.geometry);
        auto tmesh =
                dynamic_cast<const t::geometry::TriangleMesh *>(pg.tgeometry);
        if (cloud) {
            points_.insert(points_.end(), cloud->points_.begin(),
                           cloud->points_.end());
        } else if (mesh) {
            points_.insert(points_.end(), mesh->vertices_.begin(),
                           mesh->vertices_.end());
        } else if (tcloud || tmesh) {
            const auto &tpoints =
                    (tcloud ? tcloud->GetPoints() : tmesh->GetVertices());
            const size_t n = tpoints.NumElements();
            float *pts = (float *)tpoints.GetDataPtr();
            points_.reserve(points_.size() + n);
            for (size_t i = 0; i < n; i += 3) {
                points_.emplace_back(double(pts[i]), double(pts[i + 1]),
                                     double(pts[i + 2]));
            }
        }

        if (mesh || tmesh) {
            // If we draw unlit with the background color, then if the mesh is
            // drawn before the picking points the end effect is to just write
            // to the depth buffer,and if we draw after the points then we paint
            // over the occluded points.
            rendering::Material mat;
            mat.shader = "unlitSolidColor";  // ignore any vertex colors!
            mat.base_color = kBackgroundColor;
            mat.sRGB_color = false;
            if (mesh) {
                picking_scene_->AddGeometry(pg.name, mesh, mat);
            } else {
                utility::LogWarning(
                        "PickPointsInteractor::SetPickableGeometry(): "
                        "Open3DScene cannot add a t::geometry::TriangleMesh, "
                        "so points on the back side of the mesh '{}', will be "
                        "pickable",
                        pg.name);
                // picking_scene_->AddGeometry(pg.name, tmesh, mat);
            }
        }
    }

    if (points_.size() > kMaxPickableIndex) {
        utility::LogWarning(
                "Can only select from a maximumum of {} points; given {}",
                kMaxPickableIndex, points_.size());
    }

    if (!points_.empty()) {  // Filament panics if an object has zero vertices
        auto cloud = std::make_shared<geometry::PointCloud>(points_);
        cloud->colors_.reserve(points_.size());
        for (size_t i = 0; i < cloud->points_.size(); ++i) {
            cloud->colors_.emplace_back(SetColorForIndex(uint32_t(i)));
        }

        auto mat = MakeMaterial();
        picking_scene_->AddGeometry(kSelectablePointsName, cloud.get(), mat);
        picking_scene_->GetScene()->GeometryShadows(kSelectablePointsName,
                                                    false, false);
    }
}

void PickPointsInteractor::SetNeedsRedraw() {
    dirty_ = true;
    // std::queue::clear() seems to not exist
    while (!pending_.empty()) {
        pending_.pop();
    }
}

rendering::MatrixInteractorLogic &PickPointsInteractor::GetMatrixInteractor() {
    return matrix_logic_;
}

void PickPointsInteractor::SetOnPointsPicked(
        std::function<void(
                const std::map<std::string,
                               std::vector<std::pair<size_t, Eigen::Vector3d>>>
                        &,
                int)> f) {
    on_picked_ = f;
}

void PickPointsInteractor::Mouse(const MouseEvent &e) {
    if (e.type == MouseEvent::BUTTON_UP) {
        gui::Rect pick_rect(e.x, e.y, 1, 1);
        if (dirty_) {
            SetNeedsRedraw();  // note: clears pending_
            pending_.push({pick_rect, e.modifiers});
            auto *view = picking_scene_->GetView();
            view->SetViewport(0, 0,  // in case scene widget changed size
                              matrix_logic_.GetViewWidth(),
                              matrix_logic_.GetViewHeight());
            view->GetCamera()->CopyFrom(camera_);
            picking_scene_->GetRenderer().RenderToImage(
                    view, picking_scene_->GetScene(),
                    [this](std::shared_ptr<geometry::Image> img) {
                        this->OnPickImageDone(img);
#if WANT_DEBUG_IMAGE
                        std::cout << "[debug] Writing pick image to "
                                  << "/tmp/debug.png" << std::endl;
                        io::WriteImage("/tmp/debug.png", *img);
#endif  // WANT_DEBUG_IMAGE
                    });
        } else {
            pending_.push({pick_rect, e.modifiers});
            OnPickImageDone(pick_image_);
        }
    }
}

// TODO: do we need this?
void PickPointsInteractor::Key(const KeyEvent &e) {}

rendering::Material PickPointsInteractor::MakeMaterial() {
    rendering::Material mat;
    mat.shader = "unlitPolygonOffset";
    mat.point_size = float(point_size_);
    // We are not tonemapping, so src colors are RGB. This prevents the colors
    // being converted from sRGB -> linear like normal.
    mat.sRGB_color = false;
    return mat;
}

void PickPointsInteractor::OnPickImageDone(
        std::shared_ptr<geometry::Image> img) {
    if (dirty_) {
        pick_image_ = img;
        dirty_ = false;
    }

    std::map<std::string, std::vector<std::pair<size_t, Eigen::Vector3d>>>
            indices;
    while (!pending_.empty()) {
        PickInfo &info = pending_.back();
        const int x0 = info.rect.x;
        const int x1 = info.rect.GetRight();
        const int y0 = info.rect.y;
        const int y1 = info.rect.GetBottom();
        auto *img = pick_image_.get();
        indices.clear();
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                unsigned int idx = GetIndexForColor(img, x, y);
                if (idx < kNoIndex) {
                    auto &o = lookup_->ObjectForIndex(idx);
                    size_t obj_idx = idx - o.start_index;
                    indices[o.name].push_back(
                            std::pair<size_t, Eigen::Vector3d>(obj_idx,
                                                               points_[idx]));
                }
            }
        }
        pending_.pop();

        if (on_picked_ && !indices.empty()) {
            on_picked_(indices, info.keymods);
        }
    }
}

}  // namespace gui
}  // namespace visualization
}  // namespace open3d
