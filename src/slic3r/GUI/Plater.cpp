#include "Plater.hpp"

#include <cstddef>
#include <algorithm>
#include <numeric>
#include <vector>
#include <string>
#include <regex>
#include <future>
#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/bmpcbox.h>
#include <wx/statbox.h>
#include <wx/statbmp.h>
#include <wx/filedlg.h>
#include <wx/dnd.h>
#include <wx/progdlg.h>
#include <wx/wupdlock.h>

#include <wx/numdlg.h>
#include <wx/debug.h>
#include <wx/busyinfo.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/AMF.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/GCode/PreviewData.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLA/Hollowing.hpp"
#include "libslic3r/SLA/Rotfinder.hpp"
#include "libslic3r/SLA/SupportPoint.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Utils.hpp"

#include "libslic3r/MinAreaBoundingBox.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_ObjectManipulation.hpp"
#include "GUI_ObjectLayers.hpp"
#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "MainFrame.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "Selection.hpp"
#include "GLToolbar.hpp"
#include "GUI_Preview.hpp"
#include "3DBed.hpp"
#include "Camera.hpp"
#include "Mouse3DController.hpp"
#include "Tab.hpp"
#include "Job.hpp"
#include "PresetBundle.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "ProgressStatusBar.hpp"
#include "PrintHostDialogs.hpp"
#include "ConfigWizard.hpp"
#include "../Utils/ASCIIFolding.hpp"
#include "../Utils/PrintHost.hpp"
#include "../Utils/FixModelByWin10.hpp"
#include "../Utils/UndoRedo.hpp"
#include "RemovableDriveManager.hpp"

#include <wx/glcanvas.h>    // Needs to be last because reasons :-/
#include "WipeTowerDialog.hpp"
#include "libslic3r/CustomGCode.hpp"

using boost::optional;
namespace fs = boost::filesystem;
using Slic3r::_3DScene;
using Slic3r::Preset;
using Slic3r::PrintHostJob;

static const std::pair<unsigned int, unsigned int> THUMBNAIL_SIZE_3MF = { 256, 256 };

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_SLICING_UPDATE,                  SlicingStatusEvent);
wxDEFINE_EVENT(EVT_SLICING_COMPLETED,               wxCommandEvent);
wxDEFINE_EVENT(EVT_PROCESS_COMPLETED,               wxCommandEvent);

// Sidebar widgets

// struct InfoBox : public wxStaticBox
// {
//     InfoBox(wxWindow *parent, const wxString &label) :
//         wxStaticBox(parent, wxID_ANY, label)
//     {
//         SetFont(GUI::small_font().Bold());
//     }
// };



// Plater::DropTarget

class PlaterDropTarget : public wxFileDropTarget
{
public:
    PlaterDropTarget(Plater *plater) : plater(plater) { this->SetDefaultAction(wxDragCopy); }

    virtual bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString &filenames);

private:
    Plater *plater;

    static const std::regex pattern_drop;
};

const std::regex PlaterDropTarget::pattern_drop(".*[.](stl|obj|amf|3mf|prusa)", std::regex::icase);

bool PlaterDropTarget::OnDropFiles(wxCoord x, wxCoord y, const wxArrayString &filenames)
{
    std::vector<fs::path> paths;
    for (const auto &filename : filenames) {
        fs::path path(into_path(filename));
        if (std::regex_match(path.string(), pattern_drop)) {
            paths.push_back(std::move(path));
        } else {
            return false;
        }
    }

    wxString snapshot_label;
    assert(! paths.empty());
    if (paths.size() == 1) {
        snapshot_label = _(L("Load File"));
        snapshot_label += ": ";
        snapshot_label += wxString::FromUTF8(paths.front().filename().string().c_str());
    } else {
        snapshot_label = _(L("Load Files"));
        snapshot_label += ": ";
        snapshot_label += wxString::FromUTF8(paths.front().filename().string().c_str());
        for (size_t i = 1; i < paths.size(); ++ i) {
            snapshot_label += ", ";
            snapshot_label += wxString::FromUTF8(paths[i].filename().string().c_str());
        }
    }
    Plater::TakeSnapshot snapshot(plater, snapshot_label);

    // FIXME: when drag and drop is done on a .3mf or a .amf file we should clear the plater for consistence with the open project command
    // (the following call to plater->load_files() will load the config data, if present)

    std::vector<size_t> res = plater->load_files(paths);

    // because right now the plater is not cleared, we set the project file (from the latest imported .3mf or .amf file)
    // only if not set yet
    // if res is empty no data has been loaded
    if (!res.empty() && plater->get_project_filename().empty())
    {
        for (std::vector<fs::path>::const_reverse_iterator it = paths.rbegin(); it != paths.rend(); ++it)
        {
            std::string filename = (*it).filename().string();
            if (boost::algorithm::iends_with(filename, ".3mf") || boost::algorithm::iends_with(filename, ".amf"))
            {
                plater->set_project_filename(from_path(*it));
                break;
            }
        }
    }

    return true;
}

// Plater / private
struct Plater::priv
{
    // PIMPL back pointer ("Q-Pointer")
    Plater *q;
    MainFrame *main_frame;

    // Object popup menu
    MenuWithSeparators object_menu;
    // Part popup menu
    MenuWithSeparators part_menu;
    // SLA-Object popup menu
    MenuWithSeparators sla_object_menu;
    // Default popup menu (when nothing is selected on 3DScene)
    MenuWithSeparators default_menu;

    // Removed/Prepended Items according to the view mode
    std::vector<wxMenuItem*> items_increase;
    std::vector<wxMenuItem*> items_decrease;
    std::vector<wxMenuItem*> items_set_number_of_copies;
    enum MenuIdentifier {
        miObjectFFF=0,
        miObjectSLA
    };

    // Data
    Slic3r::DynamicPrintConfig *config;        // FIXME: leak?
    Slic3r::Print               fff_print;
    Slic3r::SLAPrint            sla_print;
    Slic3r::Model               model;
    PrinterTechnology           printer_technology = ptFFF;
    Slic3r::GCodePreviewData    gcode_preview_data;

    // GUI elements
    wxSizer* panel_sizer{ nullptr };
    wxPanel* current_panel{ nullptr };
    std::vector<wxPanel*> panels;
    Sidebar *sidebar;
    Bed3D bed;
    Camera camera;
    Mouse3DController mouse3d_controller;
    View3D* view3D;
    GLToolbar view_toolbar;
    Preview *preview;

    BackgroundSlicingProcess    background_process;
    bool suppressed_backround_processing_update { false };

    // Cache the wti info
    class WipeTower: public GLCanvas3D::WipeTowerInfo {
        using ArrangePolygon = arrangement::ArrangePolygon;
        friend priv;
    public:

        void apply_arrange_result(const Vec2crd& tr, double rotation)
        {
            m_pos = unscaled(tr); m_rotation = rotation;
            apply_wipe_tower();
        }

        ArrangePolygon get_arrange_polygon() const
        {
            Polygon ap({
                {coord_t(0), coord_t(0)},
                {scaled(m_bb_size(X)), coord_t(0)},
                {scaled(m_bb_size)},
                {coord_t(0), scaled(m_bb_size(Y))},
                {coord_t(0), coord_t(0)},
                });

            ArrangePolygon ret;
            ret.poly.contour = std::move(ap);
            ret.translation  = scaled(m_pos);
            ret.rotation     = m_rotation;
            ret.priority++;
            return ret;
        }
    } wipetower;

    WipeTower& updated_wipe_tower() {
        auto wti = view3D->get_canvas3d()->get_wipe_tower_info();
        wipetower.m_pos = wti.pos();
        wipetower.m_rotation = wti.rotation();
        wipetower.m_bb_size  = wti.bb_size();
        return wipetower;
    }

    // A class to handle UI jobs like arranging and optimizing rotation.
    // These are not instant jobs, the user has to be informed about their
    // state in the status progress indicator. On the other hand they are
    // separated from the background slicing process. Ideally, these jobs should
    // run when the background process is not running.
    //
    // TODO: A mechanism would be useful for blocking the plater interactions:
    // objects would be frozen for the user. In case of arrange, an animation
    // could be shown, or with the optimize orientations, partial results
    // could be displayed.
    class PlaterJob: public Job
    {
        priv *m_plater;
    protected:

        priv &      plater() { return *m_plater; }
        const priv &plater() const { return *m_plater; }

        // Launched when the job is finished. It refreshes the 3Dscene by def.
        void finalize() override
        {
            // Do a full refresh of scene tree, including regenerating
            // all the GLVolumes. FIXME The update function shall just
            // reload the modified matrices.
            if (!Job::was_canceled())
                plater().update(unsigned(UpdateParams::FORCE_FULL_SCREEN_REFRESH));
            
            Job::finalize();
        }

    public:
        PlaterJob(priv *_plater)
            : Job(_plater->statusbar()), m_plater(_plater)
        {}
    };

    enum class Jobs : size_t {
        Arrange,
        Rotoptimize
    };

    class ArrangeJob : public PlaterJob
    {
        using ArrangePolygon = arrangement::ArrangePolygon;
        using ArrangePolygons = arrangement::ArrangePolygons;

        // The gap between logical beds in the x axis expressed in ratio of
        // the current bed width.
        static const constexpr double LOGICAL_BED_GAP = 1. / 5.;

        ArrangePolygons m_selected, m_unselected, m_unprintable;

        // clear m_selected and m_unselected, reserve space for next usage
        void clear_input() {
            const Model &model = plater().model;

            size_t count = 0, cunprint = 0; // To know how much space to reserve
            for (auto obj : model.objects)
                for (auto mi : obj->instances)
                    mi->printable ? count++ : cunprint++;
            
            m_selected.clear();
            m_unselected.clear();
            m_unprintable.clear();
            m_selected.reserve(count + 1 /* for optional wti */);
            m_unselected.reserve(count + 1 /* for optional wti */);
            m_unprintable.reserve(cunprint /* for optional wti */);
        }

        // Stride between logical beds
        coord_t bed_stride() const {
            double bedwidth = plater().bed_shape_bb().size().x();
            return scaled((1. + LOGICAL_BED_GAP) * bedwidth);
        }

        // Set up arrange polygon for a ModelInstance and Wipe tower
        template<class T> ArrangePolygon get_arrange_poly(T *obj) const {
            ArrangePolygon ap = obj->get_arrange_polygon();
            ap.priority       = 0;
            ap.bed_idx        = ap.translation.x() / bed_stride();
            ap.setter         = [obj, this](const ArrangePolygon &p) {
                if (p.is_arranged()) {
                    auto t = p.translation;
                    t.x() += p.bed_idx * bed_stride();
                    obj->apply_arrange_result(t, p.rotation);
                }
            };
            return ap;
        }

        // Prepare all objects on the bed regardless of the selection
        void prepare_all() {
            clear_input();

            for (ModelObject *obj: plater().model.objects)
                for (ModelInstance *mi : obj->instances) {
                    ArrangePolygons & cont = mi->printable ? m_selected : m_unprintable;
                    cont.emplace_back(get_arrange_poly(mi));
                }

            auto& wti = plater().updated_wipe_tower();
            if (wti) m_selected.emplace_back(get_arrange_poly(&wti));
        }

        // Prepare the selected and unselected items separately. If nothing is
        // selected, behaves as if everything would be selected.
        void prepare_selected() {
            clear_input();

            Model &model = plater().model;
            coord_t stride = bed_stride();

            std::vector<const Selection::InstanceIdxsList *>
                obj_sel(model.objects.size(), nullptr);

            for (auto &s : plater().get_selection().get_content())
                if (s.first < int(obj_sel.size()))
                    obj_sel[size_t(s.first)] = &s.second;

            // Go through the objects and check if inside the selection
            for (size_t oidx = 0; oidx < model.objects.size(); ++oidx) {
                const Selection::InstanceIdxsList * instlist = obj_sel[oidx];
                ModelObject *mo = model.objects[oidx];

                std::vector<bool> inst_sel(mo->instances.size(), false);

                if (instlist)
                    for (auto inst_id : *instlist)
                        inst_sel[size_t(inst_id)] = true;

                for (size_t i = 0; i < inst_sel.size(); ++i) {
                    ArrangePolygon &&ap = get_arrange_poly(mo->instances[i]);

                    ArrangePolygons &cont = mo->instances[i]->printable ?
                                                (inst_sel[i] ? m_selected :
                                                               m_unselected) :
                                                m_unprintable;
                    
                    cont.emplace_back(std::move(ap));
                }
            }

            auto& wti = plater().updated_wipe_tower();
            if (wti) {
                ArrangePolygon &&ap = get_arrange_poly(&wti);

                plater().get_selection().is_wipe_tower() ?
                    m_selected.emplace_back(std::move(ap)) :
                    m_unselected.emplace_back(std::move(ap));
            }

            // If the selection was empty arrange everything
            if (m_selected.empty()) m_selected.swap(m_unselected);

            // The strides have to be removed from the fixed items. For the
            // arrangeable (selected) items bed_idx is ignored and the
            // translation is irrelevant.
            for (auto &p : m_unselected) p.translation(X) -= p.bed_idx * stride;
        }

    protected:

        void prepare() override
        {
            wxGetKeyState(WXK_SHIFT) ? prepare_selected() : prepare_all();
        }

    public:
        using PlaterJob::PlaterJob;

        int status_range() const override
        {
            return int(m_selected.size() + m_unprintable.size());
        }

        void process() override;

        void finalize() override {
            // Ignore the arrange result if aborted.
            if (was_canceled()) return;
            
            // Unprintable items go to the last virtual bed
            int beds = 0;
            
            // Apply the arrange result to all selected objects
            for (ArrangePolygon &ap : m_selected) {
                beds = std::max(ap.bed_idx, beds);
                ap.apply();
            }
            
            // Get the virtual beds from the unselected items
            for (ArrangePolygon &ap : m_unselected)
                beds = std::max(ap.bed_idx, beds);
            
            // Move the unprintable items to the last virtual bed.
            for (ArrangePolygon &ap : m_unprintable) {
                ap.bed_idx += beds + 1;
                ap.apply();
            }

            plater().update();
        }
    };

    class RotoptimizeJob : public PlaterJob
    {
    public:
        using PlaterJob::PlaterJob;
        void process() override;
    };


    // Jobs defined inside the group class will be managed so that only one can
    // run at a time. Also, the background process will be stopped if a job is
    // started.
    class ExclusiveJobGroup {

        static const int ABORT_WAIT_MAX_MS = 10000;

        priv * m_plater;

        ArrangeJob arrange_job{m_plater};
        RotoptimizeJob rotoptimize_job{m_plater};

        // To create a new job, just define a new subclass of Job, implement
        // the process and the optional prepare() and finalize() methods
        // Register the instance of the class in the m_jobs container
        // if it cannot run concurrently with other jobs in this group

        std::vector<std::reference_wrapper<Job>> m_jobs{arrange_job,
                                                        rotoptimize_job};

    public:
        ExclusiveJobGroup(priv *_plater) : m_plater(_plater) {}

        void start(Jobs jid) {
            m_plater->background_process.stop();
            stop_all();
            m_jobs[size_t(jid)].get().start();
        }

        void cancel_all() { for (Job& j : m_jobs) j.cancel(); }

        void join_all(int wait_ms = 0)
        {
            std::vector<bool> aborted(m_jobs.size(), false);

            for (size_t jid = 0; jid < m_jobs.size(); ++jid)
                aborted[jid] = m_jobs[jid].get().join(wait_ms);

            if (!all_of(aborted))
                BOOST_LOG_TRIVIAL(error) << "Could not abort a job!";
        }

        void stop_all() { cancel_all(); join_all(ABORT_WAIT_MAX_MS); }

        const Job& get(Jobs jobid) const { return m_jobs[size_t(jobid)]; }

        bool is_any_running() const
        {
            return std::any_of(m_jobs.begin(),
                               m_jobs.end(),
                               [](const Job &j) { return j.is_running(); });
        }

    } m_ui_jobs{this};

    bool                        delayed_scene_refresh;
    std::string                 delayed_error_message;

    wxTimer                     background_process_timer;

    std::string                 label_btn_export;
    std::string                 label_btn_send;

    static const std::regex pattern_bundle;
    static const std::regex pattern_3mf;
    static const std::regex pattern_zip_amf;
    static const std::regex pattern_any_amf;
    static const std::regex pattern_prusa;

    priv(Plater *q, MainFrame *main_frame);
    ~priv();

	enum class UpdateParams {
    	FORCE_FULL_SCREEN_REFRESH 			= 1,
    	FORCE_BACKGROUND_PROCESSING_UPDATE 	= 2,
    	POSTPONE_VALIDATION_ERROR_MESSAGE	= 4,
    };
    void update(unsigned int flags = 0);
    void select_view(const std::string& direction);
    void select_view_3D(const std::string& name);
    void select_next_view_3D();

    bool is_preview_shown() const { return current_panel == preview; }
    bool is_preview_loaded() const { return preview->is_loaded(); }
    bool is_view3D_shown() const { return current_panel == view3D; }

    bool are_view3D_labels_shown() const { return (current_panel == view3D) && view3D->get_canvas3d()->are_labels_shown(); }
    void show_view3D_labels(bool show) { if (current_panel == view3D) view3D->get_canvas3d()->show_labels(show); }

    void set_current_canvas_as_dirty();
    GLCanvas3D* get_current_canvas3D();

    bool init_view_toolbar();

    void reset_all_gizmos();
    void update_ui_from_settings();
    std::shared_ptr<ProgressStatusBar> statusbar();
    std::string get_config(const std::string &key) const;
    BoundingBoxf bed_shape_bb() const;
    BoundingBox scaled_bed_shape_bb() const;
    arrangement::BedShapeHint get_bed_shape_hint() const;

    void find_new_position(const ModelInstancePtrs  &instances, coord_t min_d);
    std::vector<size_t> load_files(const std::vector<fs::path>& input_files, bool load_model, bool load_config);
    std::vector<size_t> load_model_objects(const ModelObjectPtrs &model_objects);
    wxString get_export_file(GUI::FileType file_type);

    const Selection& get_selection() const;
    Selection& get_selection();
    int get_selected_object_idx() const;
    int get_selected_volume_idx() const;
    void selection_changed();
    void object_list_changed();

    void select_all();
    void deselect_all();
    void remove(size_t obj_idx);
    void delete_object_from_model(size_t obj_idx);
    void reset();
    void mirror(Axis axis);
    void arrange();
    void sla_optimize_rotation();
    void split_object();
    void split_volume();
    void scale_selection_to_fit_print_volume();

    // Return the active Undo/Redo stack. It may be either the main stack or the Gimzo stack.
    Slic3r::UndoRedo::Stack& undo_redo_stack() { assert(m_undo_redo_stack_active != nullptr); return *m_undo_redo_stack_active; }
    Slic3r::UndoRedo::Stack& undo_redo_stack_main() { return m_undo_redo_stack_main; }
    void enter_gizmos_stack();
    void leave_gizmos_stack();

    void take_snapshot(const std::string& snapshot_name);
    void take_snapshot(const wxString& snapshot_name) { this->take_snapshot(std::string(snapshot_name.ToUTF8().data())); }
    int  get_active_snapshot_index();

    void undo();
    void redo();
    void undo_redo_to(size_t time_to_load);

    void suppress_snapshots()   { this->m_prevent_snapshots++; }
    void allow_snapshots()      { this->m_prevent_snapshots--; }

    bool background_processing_enabled() const { return this->get_config("background_processing") == "1"; }
    void update_print_volume_state();
    void schedule_background_process();
    // Update background processing thread from the current config and Model.
    enum UpdateBackgroundProcessReturnState {
        // update_background_process() reports, that the Print / SLAPrint was updated in a way,
        // that the background process was invalidated and it needs to be re-run.
        UPDATE_BACKGROUND_PROCESS_RESTART = 1,
        // update_background_process() reports, that the Print / SLAPrint was updated in a way,
        // that a scene needs to be refreshed (you should call _3DScene::reload_scene(canvas3Dwidget, false))
        UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE = 2,
        // update_background_process() reports, that the Print / SLAPrint is invalid, and the error message
        // was sent to the status line.
        UPDATE_BACKGROUND_PROCESS_INVALID = 4,
        // Restart even if the background processing is disabled.
        UPDATE_BACKGROUND_PROCESS_FORCE_RESTART = 8,
        // Restart for G-code (or SLA zip) export or upload.
        UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT = 16,
    };
    // returns bit mask of UpdateBackgroundProcessReturnState
    unsigned int update_background_process(bool force_validation = false, bool postpone_error_messages = false);
    // Restart background processing thread based on a bitmask of UpdateBackgroundProcessReturnState.
    bool restart_background_process(unsigned int state);
    // returns bit mask of UpdateBackgroundProcessReturnState
    unsigned int update_restart_background_process(bool force_scene_update, bool force_preview_update);
	void show_delayed_error_message() {
		if (!this->delayed_error_message.empty()) {
			std::string msg = std::move(this->delayed_error_message);
			this->delayed_error_message.clear();
			GUI::show_error(this->q, msg);
		}
	}
    void export_gcode(fs::path output_path, bool output_path_on_removable_media, PrintHostJob upload_job);
    void reload_from_disk();
    void reload_all_from_disk();
    void fix_through_netfabb(const int obj_idx, const int vol_idx = -1);

    void set_current_panel(wxPanel* panel);

    void on_select_preset(wxCommandEvent&);
    void on_slicing_update(SlicingStatusEvent&);
    void on_slicing_completed(wxCommandEvent&);
    void on_process_completed(wxCommandEvent&);
    void on_layer_editing_toggled(bool enable);

    void on_action_add(SimpleEvent&);
    void on_action_split_objects(SimpleEvent&);
    void on_action_split_volumes(SimpleEvent&);
    void on_action_layersediting(SimpleEvent&);

    void on_object_select(SimpleEvent&);
    void on_right_click(RBtnEvent&);
    void on_wipetower_moved(Vec3dEvent&);
    void on_wipetower_rotated(Vec3dEvent&);
    void on_update_geometry(Vec3dsEvent<2>&);
    void on_3dcanvas_mouse_dragging_finished(SimpleEvent&);

    void update_object_menu();
    void show_action_buttons(const bool is_ready_to_slice) const;

    // Set the bed shape to a single closed 2D polygon(array of two element arrays),
    // triangulate the bed and store the triangles into m_bed.m_triangles,
    // fills the m_bed.m_grid_lines and sets m_bed.m_origin.
    // Sets m_bed.m_polygon to limit the object placement.
    void set_bed_shape(const Pointfs& shape, const std::string& custom_texture, const std::string& custom_model);

    bool can_delete() const;
    bool can_delete_all() const;
    bool can_increase_instances() const;
    bool can_decrease_instances() const;
    bool can_split_to_objects() const;
    bool can_split_to_volumes() const;
    bool can_arrange() const;
    bool can_layers_editing() const;
    bool can_fix_through_netfabb() const;
    bool can_set_instance_to_object() const;
    bool can_mirror() const;
    bool can_reload_from_disk() const;

    void generate_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, bool printable_only, bool parts_only, bool show_bed, bool transparent_background);
    void generate_thumbnails(ThumbnailsList& thumbnails, const Vec2ds& sizes, bool printable_only, bool parts_only, bool show_bed, bool transparent_background);

    void msw_rescale_object_menu();

    // returns the path to project file with the given extension (none if extension == wxEmptyString)
    // extension should contain the leading dot, i.e.: ".3mf"
    wxString get_project_filename(const wxString& extension = wxEmptyString) const;
    void set_project_filename(const wxString& filename);

    // Caching last value of show_action_buttons parameter for show_action_buttons(), so that a callback which does not know this state will not override it.
    mutable bool    			ready_to_slice = { false };
    // Flag indicating that the G-code export targets a removable device, therefore the show_action_buttons() needs to be called at any case when the background processing finishes.
    bool 						writing_to_removable_device = { false };
    bool                        inside_snapshot_capture() { return m_prevent_snapshots != 0; }

private:
    bool init_object_menu();
    bool init_common_menu(wxMenu* menu, const bool is_part = false);
    bool complit_init_object_menu();
    bool complit_init_sla_object_menu();
    bool complit_init_part_menu();

    bool can_split() const;
    bool layers_height_allowed() const;

    void update_fff_scene();
    void update_sla_scene();
    void undo_redo_to(std::vector<UndoRedo::Snapshot>::const_iterator it_snapshot);
    void update_after_undo_redo(const UndoRedo::Snapshot& snapshot, bool temp_snapshot_was_taken = false);

    // path to project file stored with no extension
    wxString 					m_project_filename;
    Slic3r::UndoRedo::Stack 	m_undo_redo_stack_main;
    Slic3r::UndoRedo::Stack 	m_undo_redo_stack_gizmos;
    Slic3r::UndoRedo::Stack    *m_undo_redo_stack_active = &m_undo_redo_stack_main;
    int                         m_prevent_snapshots = 0;     /* Used for avoid of excess "snapshoting".
                                                              * Like for "delete selected" or "set numbers of copies"
                                                              * we should call tack_snapshot just ones
                                                              * instead of calls for each action separately
                                                              * */
    std::string 				m_last_fff_printer_profile_name;
    std::string 				m_last_sla_printer_profile_name;
};

const std::regex Plater::priv::pattern_bundle(".*[.](amf|amf[.]xml|zip[.]amf|3mf|prusa)", std::regex::icase);
const std::regex Plater::priv::pattern_3mf(".*3mf", std::regex::icase);
const std::regex Plater::priv::pattern_zip_amf(".*[.]zip[.]amf", std::regex::icase);
const std::regex Plater::priv::pattern_any_amf(".*[.](amf|amf[.]xml|zip[.]amf)", std::regex::icase);
const std::regex Plater::priv::pattern_prusa(".*prusa", std::regex::icase);

Plater::priv::priv(Plater *q, MainFrame *main_frame)
    : q(q)
    , main_frame(main_frame)
    , config(Slic3r::DynamicPrintConfig::new_from_defaults_keys({
        "bed_shape", "bed_custom_texture", "bed_custom_model", "complete_objects", "duplicate_distance", "extruder_clearance_radius", "skirts", "skirt_distance",
        "brim_width", "variable_layer_height", "serial_port", "serial_speed", "host_type", "print_host",
        "printhost_apikey", "printhost_cafile", "nozzle_diameter", "single_extruder_multi_material",
        "wipe_tower", "wipe_tower_x", "wipe_tower_y", "wipe_tower_width", "wipe_tower_rotation_angle",
        "extruder_colour", "filament_colour", "max_print_height", "printer_model", "printer_technology",
        // These values are necessary to construct SlicingParameters by the Canvas3D variable layer height editor.
        "layer_height", "first_layer_height", "min_layer_height", "max_layer_height",
        "brim_width", "perimeters", "perimeter_extruder", "fill_density", "infill_extruder", "top_solid_layers", 
        "support_material", "support_material_extruder", "support_material_interface_extruder", "support_material_contact_distance", "raft_layers"
        }))
    , sidebar(new Sidebar(q))
    , delayed_scene_refresh(false)
    , view_toolbar(GLToolbar::Radio, "View")
    , m_project_filename(wxEmptyString)
{
    this->q->SetFont(Slic3r::GUI::wxGetApp().normal_font());

    background_process.set_fff_print(&fff_print);
    background_process.set_sla_print(&sla_print);
    background_process.set_gcode_preview_data(&gcode_preview_data);
    background_process.set_thumbnail_cb([this](ThumbnailsList& thumbnails, const Vec2ds& sizes, bool printable_only, bool parts_only, bool show_bed, bool transparent_background)
        {
            std::packaged_task<void(ThumbnailsList&, const Vec2ds&, bool, bool, bool, bool)> task([this](ThumbnailsList& thumbnails, const Vec2ds& sizes, bool printable_only, bool parts_only, bool show_bed, bool transparent_background) {
                generate_thumbnails(thumbnails, sizes, printable_only, parts_only, show_bed, transparent_background);
                });
            std::future<void> result = task.get_future();
            wxTheApp->CallAfter([&]() { task(thumbnails, sizes, printable_only, parts_only, show_bed, transparent_background); });
            result.wait();
        });
    background_process.set_slicing_completed_event(EVT_SLICING_COMPLETED);
    background_process.set_finished_event(EVT_PROCESS_COMPLETED);
    // Default printer technology for default config.
    background_process.select_technology(this->printer_technology);
    // Register progress callback from the Print class to the Plater.

    auto statuscb = [this](const Slic3r::PrintBase::SlicingStatus &status) {
        wxQueueEvent(this->q, new Slic3r::SlicingStatusEvent(EVT_SLICING_UPDATE, 0, status));
    };
    fff_print.set_status_callback(statuscb);
    sla_print.set_status_callback(statuscb);
    this->q->Bind(EVT_SLICING_UPDATE, &priv::on_slicing_update, this);

    view3D = new View3D(q, bed, camera, view_toolbar, &model, config, &background_process);
    preview = new Preview(q, bed, camera, view_toolbar, &model, config, &background_process, &gcode_preview_data, [this](){ schedule_background_process(); });

    panels.push_back(view3D);
    panels.push_back(preview);

    this->background_process_timer.SetOwner(this->q, 0);
    this->q->Bind(wxEVT_TIMER, [this](wxTimerEvent &evt)
    {
        if (!this->suppressed_backround_processing_update)
            this->update_restart_background_process(false, false);
    });

    update();

    auto *hsizer = new wxBoxSizer(wxHORIZONTAL);
    panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    panel_sizer->Add(view3D, 1, wxEXPAND | wxALL, 0);
    panel_sizer->Add(preview, 1, wxEXPAND | wxALL, 0);
    hsizer->Add(panel_sizer, 1, wxEXPAND | wxALL, 0);
    hsizer->Add(sidebar, 0, wxEXPAND | wxLEFT | wxRIGHT, 0);
    q->SetSizer(hsizer);

    init_object_menu();

    // Events:

    // Preset change event
    sidebar->Bind(wxEVT_COMBOBOX, &priv::on_select_preset, this);

    sidebar->Bind(EVT_OBJ_LIST_OBJECT_SELECT, [this](wxEvent&) { priv::selection_changed(); });
    sidebar->Bind(EVT_SCHEDULE_BACKGROUND_PROCESS, [this](SimpleEvent&) { this->schedule_background_process(); });

    wxGLCanvas* view3D_canvas = view3D->get_wxglcanvas();
    // 3DScene events:
    view3D_canvas->Bind(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, [this](SimpleEvent&) { this->schedule_background_process(); });
    view3D_canvas->Bind(EVT_GLCANVAS_OBJECT_SELECT, &priv::on_object_select, this);
    view3D_canvas->Bind(EVT_GLCANVAS_RIGHT_CLICK, &priv::on_right_click, this);
    view3D_canvas->Bind(EVT_GLCANVAS_REMOVE_OBJECT, [q](SimpleEvent&) { q->remove_selected(); });
    view3D_canvas->Bind(EVT_GLCANVAS_ARRANGE, [this](SimpleEvent&) { arrange(); });
    view3D_canvas->Bind(EVT_GLCANVAS_SELECT_ALL, [this](SimpleEvent&) { this->q->select_all(); });
    view3D_canvas->Bind(EVT_GLCANVAS_QUESTION_MARK, [this](SimpleEvent&) { wxGetApp().keyboard_shortcuts(); });
    view3D_canvas->Bind(EVT_GLCANVAS_INCREASE_INSTANCES, [this](Event<int> &evt)
        { if (evt.data == 1) this->q->increase_instances(); else if (this->can_decrease_instances()) this->q->decrease_instances(); });
    view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_MOVED, [this](SimpleEvent&) { update(); });
    view3D_canvas->Bind(EVT_GLCANVAS_FORCE_UPDATE, [this](SimpleEvent&) { update(); });
    view3D_canvas->Bind(EVT_GLCANVAS_WIPETOWER_MOVED, &priv::on_wipetower_moved, this);
    view3D_canvas->Bind(EVT_GLCANVAS_WIPETOWER_ROTATED, &priv::on_wipetower_rotated, this);
    view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_ROTATED, [this](SimpleEvent&) { update(); });
    view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_SCALED, [this](SimpleEvent&) { update(); });
    view3D_canvas->Bind(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, [this](Event<bool> &evt) { this->sidebar->enable_buttons(evt.data); });
    view3D_canvas->Bind(EVT_GLCANVAS_UPDATE_GEOMETRY, &priv::on_update_geometry, this);
    view3D_canvas->Bind(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, &priv::on_3dcanvas_mouse_dragging_finished, this);
    view3D_canvas->Bind(EVT_GLCANVAS_TAB, [this](SimpleEvent&) { select_next_view_3D(); });
    view3D_canvas->Bind(EVT_GLCANVAS_RESETGIZMOS, [this](SimpleEvent&) { reset_all_gizmos(); });
    view3D_canvas->Bind(EVT_GLCANVAS_UNDO, [this](SimpleEvent&) { this->undo(); });
    view3D_canvas->Bind(EVT_GLCANVAS_REDO, [this](SimpleEvent&) { this->redo(); });
    view3D_canvas->Bind(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE, [this](SimpleEvent&) { this->view3D->get_canvas3d()->reset_layer_height_profile(); });
    view3D_canvas->Bind(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, [this](Event<float>& evt) { this->view3D->get_canvas3d()->adaptive_layer_height_profile(evt.data); });
    view3D_canvas->Bind(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, [this](HeightProfileSmoothEvent& evt) { this->view3D->get_canvas3d()->smooth_layer_height_profile(evt.data); });
    view3D_canvas->Bind(EVT_GLCANVAS_RELOAD_FROM_DISK, [this](SimpleEvent&) { this->reload_all_from_disk(); });

    // 3DScene/Toolbar:
    view3D_canvas->Bind(EVT_GLTOOLBAR_ADD, &priv::on_action_add, this);
    view3D_canvas->Bind(EVT_GLTOOLBAR_DELETE, [q](SimpleEvent&) { q->remove_selected(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_DELETE_ALL, [q](SimpleEvent&) { q->reset_with_confirm(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_ARRANGE, [this](SimpleEvent&) { arrange(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_COPY, [q](SimpleEvent&) { q->copy_selection_to_clipboard(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_PASTE, [q](SimpleEvent&) { q->paste_from_clipboard(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_MORE, [q](SimpleEvent&) { q->increase_instances(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_FEWER, [q](SimpleEvent&) { q->decrease_instances(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_SPLIT_OBJECTS, &priv::on_action_split_objects, this);
    view3D_canvas->Bind(EVT_GLTOOLBAR_SPLIT_VOLUMES, &priv::on_action_split_volumes, this);
    view3D_canvas->Bind(EVT_GLTOOLBAR_LAYERSEDITING, &priv::on_action_layersediting, this);
    view3D_canvas->Bind(EVT_GLCANVAS_UPDATE_BED_SHAPE, [this](SimpleEvent&)
        {
            set_bed_shape(config->option<ConfigOptionPoints>("bed_shape")->values,
                config->option<ConfigOptionString>("bed_custom_texture")->value,
                config->option<ConfigOptionString>("bed_custom_model")->value);
        });

    // Preview events:
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_QUESTION_MARK, [this](SimpleEvent&) { wxGetApp().keyboard_shortcuts(); });
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_UPDATE_BED_SHAPE, [this](SimpleEvent&)
        {
            set_bed_shape(config->option<ConfigOptionPoints>("bed_shape")->values,
                config->option<ConfigOptionString>("bed_custom_texture")->value,
                config->option<ConfigOptionString>("bed_custom_model")->value);
        });
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_TAB, [this](SimpleEvent&) { select_next_view_3D(); });
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_MOVE_DOUBLE_SLIDER, [this](wxKeyEvent& evt) { preview->move_double_slider(evt); });
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_EDIT_COLOR_CHANGE, [this](wxKeyEvent& evt) { preview->edit_double_slider(evt); });

    q->Bind(EVT_SLICING_COMPLETED, &priv::on_slicing_completed, this);
    q->Bind(EVT_PROCESS_COMPLETED, &priv::on_process_completed, this);
    q->Bind(EVT_GLVIEWTOOLBAR_3D, [q](SimpleEvent&) { q->select_view_3D("3D"); });
    q->Bind(EVT_GLVIEWTOOLBAR_PREVIEW, [q](SimpleEvent&) { q->select_view_3D("Preview"); });

    // Drop target:
    q->SetDropTarget(new PlaterDropTarget(q));   // if my understanding is right, wxWindow takes the owenership

    update_ui_from_settings();
    q->Layout();

    set_current_panel(view3D);

    // updates camera type from .ini file
    camera.set_type(get_config("use_perspective_camera"));

    // Load the 3DConnexion device database.
    mouse3d_controller.load_config(*wxGetApp().app_config);
	// Start the background thread to detect and connect to a HID device (Windows and Linux).
	// Connect to a 3DConnextion driver (OSX).
    mouse3d_controller.init();
#ifdef _WIN32
    // Register an USB HID (Human Interface Device) attach event. evt contains Win32 path to the USB device containing VID, PID and other info.
    // This event wakes up the Mouse3DController's background thread to enumerate HID devices, if the VID of the callback event
    // is one of the 3D Mouse vendors (3DConnexion or Logitech).
    this->q->Bind(EVT_HID_DEVICE_ATTACHED, [this](HIDDeviceAttachedEvent &evt) {
    	mouse3d_controller.device_attached(evt.data);
    });
#endif /* _WIN32 */

    this->q->Bind(EVT_REMOVABLE_DRIVE_EJECTED, [this](RemovableDriveEjectEvent &evt) {
		if (evt.data.second) {
			this->show_action_buttons(this->ready_to_slice);
			Slic3r::GUI::show_info(this->q, (boost::format(_utf8(L("Unmounting successful. The device %s(%s) can now be safely removed from the computer.")))
				% evt.data.first.name % evt.data.first.path).str());
		} else
			Slic3r::GUI::show_info(this->q, (boost::format(_utf8(L("Ejecting of device %s(%s) has failed.")))
				% evt.data.first.name % evt.data.first.path).str());
	});
    this->q->Bind(EVT_REMOVABLE_DRIVES_CHANGED, [this](RemovableDrivesChangedEvent &) { this->show_action_buttons(this->ready_to_slice); });
    // Start the background thread and register this window as a target for update events.
    wxGetApp().removable_drive_manager()->init(this->q);
#ifdef _WIN32
    // Trigger enumeration of removable media on Win32 notification.
    this->q->Bind(EVT_VOLUME_ATTACHED, [this](VolumeAttachedEvent &evt) { wxGetApp().removable_drive_manager()->volumes_changed(); });
    this->q->Bind(EVT_VOLUME_DETACHED, [this](VolumeDetachedEvent &evt) { wxGetApp().removable_drive_manager()->volumes_changed(); });
#endif /* _WIN32 */

    // Initialize the Undo / Redo stack with a first snapshot.
    this->take_snapshot(_(L("New Project")));
}

Plater::priv::~priv()
{
    if (config != nullptr)
        delete config;
}

void Plater::priv::update(unsigned int flags)
{
    // the following line, when enabled, causes flickering on NVIDIA graphics cards
//    wxWindowUpdateLocker freeze_guard(q);
    if (get_config("autocenter") == "1") {
        // auto *bed_shape_opt = config->opt<ConfigOptionPoints>("bed_shape");
        // const auto bed_shape = Slic3r::Polygon::new_scale(bed_shape_opt->values);
        // const BoundingBox bed_shape_bb = bed_shape.bounding_box();
        const Vec2d& bed_center = bed_shape_bb().center();
        model.center_instances_around_point(bed_center);
    }

    unsigned int update_status = 0;
    if (this->printer_technology == ptSLA || (flags & (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE))
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data.
        update_status = this->update_background_process(false, flags & (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE);
    this->view3D->reload_scene(false, flags & (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH);
    this->preview->reload_print();
    if (this->printer_technology == ptSLA)
        this->restart_background_process(update_status);
    else
        this->schedule_background_process();
}

void Plater::priv::select_view(const std::string& direction)
{
    if (current_panel == view3D)
        view3D->select_view(direction);
    else if (current_panel == preview)
        preview->select_view(direction);
}

void Plater::priv::select_view_3D(const std::string& name)
{
    if (name == "3D")
        set_current_panel(view3D);
    else if (name == "Preview")
        set_current_panel(preview);
}

void Plater::priv::select_next_view_3D()
{
    if (current_panel == view3D)
        set_current_panel(preview);
    else if (current_panel == preview)
        set_current_panel(view3D);
}

void Plater::priv::reset_all_gizmos()
{
    view3D->get_canvas3d()->reset_all_gizmos();
}

// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.
void Plater::priv::update_ui_from_settings()
{
    camera.set_type(wxGetApp().app_config->get("use_perspective_camera"));
    if (wxGetApp().app_config->get("use_free_camera") != "1")
        camera.recover_from_free_camera();

    view3D->get_canvas3d()->update_ui_from_settings();
    preview->get_canvas3d()->update_ui_from_settings();
}

std::shared_ptr<ProgressStatusBar> Plater::priv::statusbar()
{
    return main_frame->m_statusbar;
}

std::string Plater::priv::get_config(const std::string &key) const
{
    return wxGetApp().app_config->get(key);
}

BoundingBoxf Plater::priv::bed_shape_bb() const
{
    BoundingBox bb = scaled_bed_shape_bb();
    return BoundingBoxf(unscale(bb.min), unscale(bb.max));
}

BoundingBox Plater::priv::scaled_bed_shape_bb() const
{
    const auto *bed_shape_opt = config->opt<ConfigOptionPoints>("bed_shape");
    const auto bed_shape = Slic3r::Polygon::new_scale(bed_shape_opt->values);
    return bed_shape.bounding_box();
}

std::vector<size_t> Plater::priv::load_files(const std::vector<fs::path>& input_files, bool load_model, bool load_config)
{
    if (input_files.empty()) { return std::vector<size_t>(); }

    auto *nozzle_dmrs = config->opt<ConfigOptionFloats>("nozzle_diameter");

    bool one_by_one = input_files.size() == 1 || printer_technology == ptSLA || nozzle_dmrs->values.size() <= 1;
    if (! one_by_one) {
        for (const auto &path : input_files) {
            if (std::regex_match(path.string(), pattern_bundle)) {
                one_by_one = true;
                break;
            }
        }
    }

    const auto loading = _(L("Loading")) + dots;
    wxProgressDialog dlg(loading, loading);
    dlg.Pulse();

    auto *new_model = (!load_model || one_by_one) ? nullptr : new Slic3r::Model();
    std::vector<size_t> obj_idxs;

    for (size_t i = 0; i < input_files.size(); i++) {
        const auto &path = input_files[i];
        const auto filename = path.filename();
        const auto dlg_info = from_u8((boost::format(_utf8(L("Processing input file %s"))) % from_path(filename)).str()) + "\n";
        dlg.Update(100 * i / input_files.size(), dlg_info);

        const bool type_3mf = std::regex_match(path.string(), pattern_3mf);
        const bool type_zip_amf = !type_3mf && std::regex_match(path.string(), pattern_zip_amf);
        const bool type_any_amf = !type_3mf && std::regex_match(path.string(), pattern_any_amf);
        const bool type_prusa = std::regex_match(path.string(), pattern_prusa);

        Slic3r::Model model;
        bool is_project_file = type_prusa;
        try {
            if (type_3mf || type_zip_amf) {
                DynamicPrintConfig config;
                {
                    DynamicPrintConfig config_loaded;
                    model = Slic3r::Model::read_from_archive(path.string(), &config_loaded, false, load_config);
                    if (load_config && !config_loaded.empty()) {
                        // Based on the printer technology field found in the loaded config, select the base for the config,
                        PrinterTechnology printer_technology = Preset::printer_technology(config_loaded);

                        // We can't to load SLA project if there is at least one multi-part object on the bed
                        if (printer_technology == ptSLA)
                        {
                            const ModelObjectPtrs& objects = q->model().objects;
                            for (auto object : objects)
                                if (object->volumes.size() > 1)
                                {
                                    Slic3r::GUI::show_info(nullptr,
                                        _(L("You cannot load SLA project with a multi-part object on the bed")) + "\n\n" +
                                        _(L("Please check your object list before preset changing.")),
                                        _(L("Attention!")));
                                    return obj_idxs;
                                }
                        }

                        config.apply(printer_technology == ptFFF ?
                            static_cast<const ConfigBase&>(FullPrintConfig::defaults()) :
                            static_cast<const ConfigBase&>(SLAFullPrintConfig::defaults()));
                        // and place the loaded config over the base.
                        config += std::move(config_loaded);
                    }

                    this->model.custom_gcode_per_print_z = model.custom_gcode_per_print_z;
                }

                if (load_config)
                {
                    if (!config.empty()) {
                        Preset::normalize(config);
                        wxGetApp().preset_bundle->load_config_model(filename.string(), std::move(config));
                        wxGetApp().load_current_presets();
                        is_project_file = true;
                    }
                    wxGetApp().app_config->update_config_dir(path.parent_path().string());
                }
            }
            else {
                model = Slic3r::Model::read_from_file(path.string(), nullptr, false, load_config);
                for (auto obj : model.objects)
                    if (obj->name.empty())
                        obj->name = fs::path(obj->input_file).filename().string();
            }
        } catch (const std::exception &e) {
            GUI::show_error(q, e.what());
            continue;
        }

        if (load_model)
        {
            // The model should now be initialized

            if (! is_project_file) {
                if (model.looks_like_multipart_object()) {
                    wxMessageDialog msg_dlg(q, _(L(
                        "This file contains several objects positioned at multiple heights.\n"
                        "Instead of considering them as multiple objects, should I consider\n"
                        "this file as a single object having multiple parts?")) + "\n",
                        _(L("Multi-part object detected")), wxICON_WARNING | wxYES | wxNO);
                    if (msg_dlg.ShowModal() == wxID_YES) {
                        model.convert_multipart_object(nozzle_dmrs->values.size());
                    }
                }
            }
            else if ((wxGetApp().get_mode() == comSimple) && (type_3mf || type_any_amf) && model_has_advanced_features(model)) {
                wxMessageDialog msg_dlg(q, _(L("This file cannot be loaded in a simple mode. Do you want to switch to an advanced mode?"))+"\n",
                    _(L("Detected advanced data")), wxICON_WARNING | wxYES | wxNO);
                if (msg_dlg.ShowModal() == wxID_YES)
                {
                    Slic3r::GUI::wxGetApp().save_mode(comAdvanced);
                    view3D->set_as_dirty();
                }
                else
                    return obj_idxs;
            }

            for (ModelObject* model_object : model.objects) {
                if (!type_3mf && !type_zip_amf)
                    model_object->center_around_origin(false);
                model_object->ensure_on_bed();
            }

            // check multi-part object adding for the SLA-printing
            if (printer_technology == ptSLA)
            {
                for (auto obj : model.objects)
                    if ( obj->volumes.size()>1 ) {
                        Slic3r::GUI::show_error(nullptr,
                            from_u8((boost::format(_utf8(L("You can't to add the object(s) from %s because of one or some of them is(are) multi-part")))
                                             % from_path(filename)).str()));
                        return obj_idxs;
                    }
            }

            if (one_by_one) {
                auto loaded_idxs = load_model_objects(model.objects);
                obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());
            } else {
                // This must be an .stl or .obj file, which may contain a maximum of one volume.
                for (const ModelObject* model_object : model.objects) {
                    new_model->add_object(*model_object);
                }
            }
        }
    }

    if (new_model != nullptr && new_model->objects.size() > 1) {
        wxMessageDialog msg_dlg(q, _(L(
                "Multiple objects were loaded for a multi-material printer.\n"
                "Instead of considering them as multiple objects, should I consider\n"
                "these files to represent a single object having multiple parts?")) + "\n",
                _(L("Multi-part object detected")), wxICON_WARNING | wxYES | wxNO);
        if (msg_dlg.ShowModal() == wxID_YES) {
            new_model->convert_multipart_object(nozzle_dmrs->values.size());
        }

        auto loaded_idxs = load_model_objects(new_model->objects);
        obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());
    }

    if (load_model)
    {
        wxGetApp().app_config->update_skein_dir(input_files[input_files.size() - 1].parent_path().string());
        // XXX: Plater.pm had @loaded_files, but didn't seem to fill them with the filenames...
        statusbar()->set_status_text(_(L("Loaded")));
    }

    // automatic selection of added objects
    if (!obj_idxs.empty() && (view3D != nullptr))
    {
        // update printable state for new volumes on canvas3D
        wxGetApp().plater()->canvas3D()->update_instance_printable_state_for_objects(obj_idxs);

        Selection& selection = view3D->get_canvas3d()->get_selection();
        selection.clear();
        for (size_t idx : obj_idxs)
        {
            selection.add_object((unsigned int)idx, false);
        }

        if (view3D->get_canvas3d()->get_gizmos_manager().is_enabled())
            // this is required because the selected object changed and the flatten on face an sla support gizmos need to be updated accordingly
            view3D->get_canvas3d()->update_gizmos_on_off_state();
    }

    return obj_idxs;
}

// #define AUTOPLACEMENT_ON_LOAD

std::vector<size_t> Plater::priv::load_model_objects(const ModelObjectPtrs &model_objects)
{
    const BoundingBoxf bed_shape = bed_shape_bb();
    const Vec3d bed_size = Slic3r::to_3d(bed_shape.size().cast<double>(), 1.0) - 2.0 * Vec3d::Ones();

#ifndef AUTOPLACEMENT_ON_LOAD
    bool need_arrange = false;
#endif /* AUTOPLACEMENT_ON_LOAD */
    bool scaled_down = false;
    std::vector<size_t> obj_idxs;
    unsigned int obj_count = model.objects.size();

#ifdef AUTOPLACEMENT_ON_LOAD
    ModelInstancePtrs new_instances;
#endif /* AUTOPLACEMENT_ON_LOAD */
    for (ModelObject *model_object : model_objects) {
        auto *object = model.add_object(*model_object);
        std::string object_name = object->name.empty() ? fs::path(object->input_file).filename().string() : object->name;
        obj_idxs.push_back(obj_count++);

        if (model_object->instances.empty()) {
#ifdef AUTOPLACEMENT_ON_LOAD
            object->center_around_origin();
            new_instances.emplace_back(object->add_instance());
#else /* AUTOPLACEMENT_ON_LOAD */
            // if object has no defined position(s) we need to rearrange everything after loading
            need_arrange = true;
             // add a default instance and center object around origin
            object->center_around_origin();  // also aligns object to Z = 0
            ModelInstance* instance = object->add_instance();
            instance->set_offset(Slic3r::to_3d(bed_shape.center().cast<double>(), -object->origin_translation(2)));
#endif /* AUTOPLACEMENT_ON_LOAD */
        }

        const Vec3d size = object->bounding_box().size();
        const Vec3d ratio = size.cwiseQuotient(bed_size);
        const double max_ratio = std::max(ratio(0), ratio(1));
        if (max_ratio > 10000) {
            // the size of the object is too big -> this could lead to overflow when moving to clipper coordinates,
            // so scale down the mesh
            double inv = 1. / max_ratio;
            object->scale_mesh_after_creation(Vec3d(inv, inv, inv));
            object->origin_translation = Vec3d::Zero();
            object->center_around_origin();
            scaled_down = true;
        } else if (max_ratio > 5) {
            const Vec3d inverse = 1.0 / max_ratio * Vec3d::Ones();
            for (ModelInstance *instance : object->instances) {
                instance->set_scaling_factor(inverse);
            }
            scaled_down = true;
        }

        object->ensure_on_bed();
    }

#ifdef AUTOPLACEMENT_ON_LOAD
    // FIXME distance should be a config value /////////////////////////////////
    auto min_obj_distance = static_cast<coord_t>(6/SCALING_FACTOR);
    const auto *bed_shape_opt = config->opt<ConfigOptionPoints>("bed_shape");
    assert(bed_shape_opt);
    auto& bedpoints = bed_shape_opt->values;
    Polyline bed; bed.points.reserve(bedpoints.size());
    for(auto& v : bedpoints) bed.append(Point::new_scale(v(0), v(1)));

    std::pair<bool, GLCanvas3D::WipeTowerInfo> wti = view3D->get_canvas3d()->get_wipe_tower_info();

    arr::find_new_position(model, new_instances, min_obj_distance, bed, wti);

    // it remains to move the wipe tower:
    view3D->get_canvas3d()->arrange_wipe_tower(wti);

#endif /* AUTOPLACEMENT_ON_LOAD */

    if (scaled_down) {
        GUI::show_info(q,
            _(L("Your object appears to be too large, so it was automatically scaled down to fit your print bed.")),
            _(L("Object too large?")));
    }

    for (const size_t idx : obj_idxs) {
        wxGetApp().obj_list()->add_object_to_list(idx);
    }

    update();
    object_list_changed();

    this->schedule_background_process();

    return obj_idxs;
}

wxString Plater::priv::get_export_file(GUI::FileType file_type)
{
    wxString wildcard;
    switch (file_type) {
        case FT_STL:
        case FT_AMF:
        case FT_3MF:
        case FT_GCODE:
        case FT_OBJ:
            wildcard = file_wildcards(file_type);
        break;
        default:
            wildcard = file_wildcards(FT_MODEL);
        break;
    }

    // Update printbility state of each of the ModelInstances.
    this->update_print_volume_state();

    const Selection& selection = get_selection();
    int obj_idx = selection.get_object_idx();

    fs::path output_file;
    if (file_type == FT_3MF)
        // for 3mf take the path from the project filename, if any
        output_file = into_path(get_project_filename(".3mf"));

    if (output_file.empty())
    {
        // first try to get the file name from the current selection
        if ((0 <= obj_idx) && (obj_idx < (int)this->model.objects.size()))
            output_file = this->model.objects[obj_idx]->get_export_filename();

        if (output_file.empty())
            // Find the file name of the first printable object.
            output_file = this->model.propose_export_file_name_and_path();

        if (output_file.empty() && !model.objects.empty())
            // Find the file name of the first object.
            output_file = this->model.objects[0]->get_export_filename();
    }

    wxString dlg_title;
    switch (file_type) {
        case FT_STL:
        {
            output_file.replace_extension("stl");
            dlg_title = _(L("Export STL file:"));
            break;
        }
        case FT_AMF:
        {
            // XXX: Problem on OS X with double extension?
            output_file.replace_extension("zip.amf");
            dlg_title = _(L("Export AMF file:"));
            break;
        }
        case FT_3MF:
        {
            output_file.replace_extension("3mf");
            dlg_title = _(L("Save file as:"));
            break;
        }
        case FT_OBJ:
        {
            output_file.replace_extension("obj");
            dlg_title = _(L("Export OBJ file:"));
            break;
        }
        default: break;
    }

    wxFileDialog dlg(q, dlg_title,
        from_path(output_file.parent_path()), from_path(output_file.filename()),
        wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    if (dlg.ShowModal() != wxID_OK)
        return wxEmptyString;

    wxString out_path = dlg.GetPath();
    fs::path path(into_path(out_path));
    wxGetApp().app_config->update_last_output_dir(path.parent_path().string());

    return out_path;
}

const Selection& Plater::priv::get_selection() const
{
    return view3D->get_canvas3d()->get_selection();
}

Selection& Plater::priv::get_selection()
{
    return view3D->get_canvas3d()->get_selection();
}

int Plater::priv::get_selected_object_idx() const
{
    int idx = get_selection().get_object_idx();
    return ((0 <= idx) && (idx < 1000)) ? idx : -1;
}

int Plater::priv::get_selected_volume_idx() const
{
    auto& selection = get_selection();
    int idx = selection.get_object_idx();
    if ((0 > idx) || (idx > 1000))
        return-1;
    const GLVolume* v = selection.get_volume(*selection.get_volume_idxs().begin());
    if (model.objects[idx]->volumes.size() > 1)
        return v->volume_idx();
    return -1;
}

void Plater::priv::selection_changed()
{
    // if the selection is not valid to allow for layer editing, we need to turn off the tool if it is running
    bool enable_layer_editing = layers_height_allowed();
    if (!enable_layer_editing && view3D->is_layers_editing_enabled()) {
        SimpleEvent evt(EVT_GLTOOLBAR_LAYERSEDITING);
        on_action_layersediting(evt);
    }

    // forces a frame render to update the view (to avoid a missed update if, for example, the context menu appears)
    view3D->render();
}

void Plater::priv::object_list_changed()
{
    const bool export_in_progress = this->background_process.is_export_scheduled(); // || ! send_gcode_file.empty());
    // XXX: is this right?
    const bool model_fits = view3D->check_volumes_outside_state() == ModelInstance::PVS_Inside;

    sidebar->enable_buttons(!model.objects.empty() && !export_in_progress && model_fits);
}

void Plater::priv::select_all()
{
//    this->take_snapshot(_(L("Select All")));

    view3D->select_all();
    this->sidebar->obj_list()->update_selections();
}

void Plater::priv::deselect_all()
{
//    this->take_snapshot(_(L("Deselect All")));
    view3D->deselect_all();
}

void Plater::priv::remove(size_t obj_idx)
{
    // Prevent toolpaths preview from rendering while we modify the Print object
    preview->set_enabled(false);

    if (view3D->is_layers_editing_enabled())
        view3D->enable_layers_editing(false);

    model.delete_object(obj_idx);
    update();
    // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
    sidebar->obj_list()->delete_object_from_list(obj_idx);
    object_list_changed();
}


void Plater::priv::delete_object_from_model(size_t obj_idx)
{
    wxString snapshot_label = _(L("Delete Object"));
    if (! model.objects[obj_idx]->name.empty())
        snapshot_label += ": " + wxString::FromUTF8(model.objects[obj_idx]->name.c_str());
    Plater::TakeSnapshot snapshot(q, snapshot_label);
    model.delete_object(obj_idx);
    update();
    object_list_changed();
}

void Plater::priv::reset()
{
    Plater::TakeSnapshot snapshot(q, _(L("Reset Project")));

    set_project_filename(wxEmptyString);

    // Prevent toolpaths preview from rendering while we modify the Print object
    preview->set_enabled(false);

    if (view3D->is_layers_editing_enabled())
        view3D->enable_layers_editing(false);

    // Stop and reset the Print content.
    this->background_process.reset();
    model.clear_objects();
    update();
    // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
    sidebar->obj_list()->delete_all_objects_from_list();
    object_list_changed();

    // The hiding of the slicing results, if shown, is not taken care by the background process, so we do it here
    this->sidebar->show_sliced_info_sizer(false);

    model.custom_gcode_per_print_z.gcodes.clear();
}

void Plater::priv::mirror(Axis axis)
{
    view3D->mirror_selection(axis);
}

void Plater::priv::arrange()
{
    this->take_snapshot(_(L("Arrange")));
    m_ui_jobs.start(Jobs::Arrange);
}


// This method will find an optimal orientation for the currently selected item
// Very similar in nature to the arrange method above...
void Plater::priv::sla_optimize_rotation() {
    this->take_snapshot(_(L("Optimize Rotation")));
    m_ui_jobs.start(Jobs::Rotoptimize);
}

arrangement::BedShapeHint Plater::priv::get_bed_shape_hint() const {

    const auto *bed_shape_opt = config->opt<ConfigOptionPoints>("bed_shape");
    assert(bed_shape_opt);

    if (!bed_shape_opt) return {};

    auto &bedpoints = bed_shape_opt->values;
    Polyline bedpoly; bedpoly.points.reserve(bedpoints.size());
    for (auto &v : bedpoints) bedpoly.append(scaled(v));

    return arrangement::BedShapeHint(bedpoly);
}

void Plater::priv::find_new_position(const ModelInstancePtrs &instances,
                                     coord_t min_d)
{
    arrangement::ArrangePolygons movable, fixed;

    for (const ModelObject *mo : model.objects)
        for (const ModelInstance *inst : mo->instances) {
            auto it = std::find(instances.begin(), instances.end(), inst);
            auto arrpoly = inst->get_arrange_polygon();

            if (it == instances.end())
                fixed.emplace_back(std::move(arrpoly));
            else
                movable.emplace_back(std::move(arrpoly));
        }

    if (updated_wipe_tower())
        fixed.emplace_back(wipetower.get_arrange_polygon());

    arrangement::arrange(movable, fixed, min_d, get_bed_shape_hint());

    for (size_t i = 0; i < instances.size(); ++i)
        if (movable[i].bed_idx == 0)
            instances[i]->apply_arrange_result(movable[i].translation,
                                               movable[i].rotation);
}

void Plater::priv::ArrangeJob::process() {
    static const auto arrangestr = _(L("Arranging"));

    double dist = min_object_distance(*plater().config);
    
    coord_t min_d = scaled(dist);
    auto count = unsigned(m_selected.size() + m_unprintable.size());
    arrangement::BedShapeHint bedshape = plater().get_bed_shape_hint();
    
    auto stopfn = [this]() { return was_canceled(); };
    
    try {
        arrangement::arrange(m_selected, m_unselected, min_d, bedshape,
            [this, count](unsigned st) {
                st += m_unprintable.size();
                if (st > 0) update_status(int(count - st), arrangestr);
            }, stopfn);
        arrangement::arrange(m_unprintable, {}, min_d, bedshape,
            [this, count](unsigned st) {
                if (st > 0) update_status(int(count - st), arrangestr);
            }, stopfn);
    } catch (std::exception & /*e*/) {
        GUI::show_error(plater().q,
                        _(L("Could not arrange model objects! "
                            "Some geometries may be invalid.")));
    }

    // finalize just here.
    update_status(int(count),
                  was_canceled() ? _(L("Arranging canceled."))
                                 : _(L("Arranging done.")));
}

void Plater::priv::RotoptimizeJob::process()
{
    int obj_idx = plater().get_selected_object_idx();
    if (obj_idx < 0) { return; }

    ModelObject *o = plater().model.objects[size_t(obj_idx)];

    auto r = sla::find_best_rotation(
        *o,
        .005f,
        [this](unsigned s) {
            if (s < 100)
                update_status(int(s),
                              _(L("Searching for optimal orientation")));
        },
        [this]() { return was_canceled(); });


    double mindist = 6.0; // FIXME

    if (!was_canceled()) {
        for(ModelInstance * oi : o->instances) {
            oi->set_rotation({r[X], r[Y], r[Z]});

            auto    trmatrix = oi->get_transformation().get_matrix();
            Polygon trchull  = o->convex_hull_2d(trmatrix);

            MinAreaBoundigBox rotbb(trchull, MinAreaBoundigBox::pcConvex);
            double            r = rotbb.angle_to_X();

            // The box should be landscape
            if(rotbb.width() < rotbb.height()) r += PI / 2;

            Vec3d rt = oi->get_rotation(); rt(Z) += r;

            oi->set_rotation(rt);
        }

        plater().find_new_position(o->instances, scaled(mindist));

        // Correct the z offset of the object which was corrupted be
        // the rotation
        o->ensure_on_bed();
    }

    update_status(100,
                  was_canceled() ? _(L("Orientation search canceled."))
                                 : _(L("Orientation found.")));
}


void Plater::priv::split_object()
{
    int obj_idx = get_selected_object_idx();
    if (obj_idx == -1)
        return;
    
    // we clone model object because split_object() adds the split volumes
    // into the same model object, thus causing duplicates when we call load_model_objects()
    Model new_model = model;
    ModelObject* current_model_object = new_model.objects[obj_idx];

    if (current_model_object->volumes.size() > 1)
    {
        Slic3r::GUI::warning_catcher(q, _(L("The selected object can't be split because it contains more than one volume/material.")));
        return;
    }

    wxBusyCursor wait;
    ModelObjectPtrs new_objects;
    current_model_object->split(&new_objects);
    if (new_objects.size() == 1)
        Slic3r::GUI::warning_catcher(q, _(L("The selected object couldn't be split because it contains only one part.")));
    else
    {
        Plater::TakeSnapshot snapshot(q, _(L("Split to Objects")));

        unsigned int counter = 1;
        for (ModelObject* m : new_objects)
            m->name = current_model_object->name + "_" + std::to_string(counter++);

        remove(obj_idx);

        // load all model objects at once, otherwise the plate would be rearranged after each one
        // causing original positions not to be kept
        std::vector<size_t> idxs = load_model_objects(new_objects);

        // select newly added objects
        for (size_t idx : idxs)
        {
            get_selection().add_object((unsigned int)idx, false);
        }
    }
}

void Plater::priv::split_volume()
{
    wxGetApp().obj_list()->split();
}

void Plater::priv::scale_selection_to_fit_print_volume()
{
    this->view3D->get_canvas3d()->get_selection().scale_to_fit_print_volume(*config);
}

void Plater::priv::schedule_background_process()
{
    delayed_error_message.clear();
    // Trigger the timer event after 0.5s
    this->background_process_timer.Start(500, wxTIMER_ONE_SHOT);
    // Notify the Canvas3D that something has changed, so it may invalidate some of the layer editing stuff.
    this->view3D->get_canvas3d()->set_config(this->config);
}

void Plater::priv::update_print_volume_state()
{
    BoundingBox     bed_box_2D = get_extents(Polygon::new_scale(this->config->opt<ConfigOptionPoints>("bed_shape")->values));
    BoundingBoxf3   print_volume(unscale(bed_box_2D.min(0), bed_box_2D.min(1), 0.0), unscale(bed_box_2D.max(0), bed_box_2D.max(1), scale_(this->config->opt_float("max_print_height"))));
    // Allow the objects to protrude below the print bed, only the part of the object above the print bed will be sliced.
    print_volume.min(2) = -1e10;
    this->q->model().update_print_volume_state(print_volume);
}

// Update background processing thread from the current config and Model.
// Returns a bitmask of UpdateBackgroundProcessReturnState.
unsigned int Plater::priv::update_background_process(bool force_validation, bool postpone_error_messages)
{
    // bitmap of enum UpdateBackgroundProcessReturnState
    unsigned int return_state = 0;

    // If the update_background_process() was not called by the timer, kill the timer,
    // so the update_restart_background_process() will not be called again in vain.
    this->background_process_timer.Stop();
    // Update the "out of print bed" state of ModelInstances.
    this->update_print_volume_state();
    // Apply new config to the possibly running background task.
    bool               was_running = this->background_process.running();
    Print::ApplyStatus invalidated = this->background_process.apply(this->q->model(), wxGetApp().preset_bundle->full_config());

    // Just redraw the 3D canvas without reloading the scene to consume the update of the layer height profile.
    if (view3D->is_layers_editing_enabled())
        view3D->get_wxglcanvas()->Refresh();

    if (invalidated == Print::APPLY_STATUS_INVALIDATED) {
        // Some previously calculated data on the Print was invalidated.
        // Hide the slicing results, as the current slicing status is no more valid.
        this->sidebar->show_sliced_info_sizer(false);
        // Reset preview canvases. If the print has been invalidated, the preview canvases will be cleared.
        // Otherwise they will be just refreshed.
        if (this->preview != nullptr)
            // If the preview is not visible, the following line just invalidates the preview,
            // but the G-code paths or SLA preview are calculated first once the preview is made visible.
            this->preview->reload_print();
        // In FDM mode, we need to reload the 3D scene because of the wipe tower preview box.
        // In SLA mode, we need to reload the 3D scene every time to show the support structures.
        if (this->printer_technology == ptSLA || (this->printer_technology == ptFFF && this->config->opt_bool("wipe_tower")))
            return_state |= UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE;
    }

    if ((invalidated != Print::APPLY_STATUS_UNCHANGED || force_validation) && ! this->background_process.empty()) {
		// The delayed error message is no more valid.
		this->delayed_error_message.clear();
		// The state of the Print changed, and it is non-zero. Let's validate it and give the user feedback on errors.
        std::string err = this->background_process.validate();
        if (err.empty()) {
            if (invalidated != Print::APPLY_STATUS_UNCHANGED && this->background_processing_enabled())
                return_state |= UPDATE_BACKGROUND_PROCESS_RESTART;
        } else {
            // The print is not valid.
            // Only show the error message immediately, if the top level parent of this window is active.
            auto p = dynamic_cast<wxWindow*>(this->q);
            while (p->GetParent())
                p = p->GetParent();
            auto *top_level_wnd = dynamic_cast<wxTopLevelWindow*>(p);
            if (! postpone_error_messages && top_level_wnd && top_level_wnd->IsActive()) {
                // The error returned from the Print needs to be translated into the local language.
                GUI::show_error(this->q, err);
            } else {
                // Show the error message once the main window gets activated.
                this->delayed_error_message = err;
            }
            return_state |= UPDATE_BACKGROUND_PROCESS_INVALID;
        }
    } else if (! this->delayed_error_message.empty()) {
    	// Reusing the old state.
        return_state |= UPDATE_BACKGROUND_PROCESS_INVALID;
    }

    if (invalidated != Print::APPLY_STATUS_UNCHANGED && was_running && ! this->background_process.running() &&
        (return_state & UPDATE_BACKGROUND_PROCESS_RESTART) == 0) {
        // The background processing was killed and it will not be restarted.
        wxCommandEvent evt(EVT_PROCESS_COMPLETED);
        evt.SetInt(-1);
        // Post the "canceled" callback message, so that it will be processed after any possible pending status bar update messages.
        wxQueueEvent(GUI::wxGetApp().mainframe->m_plater, evt.Clone());
    }

    if ((return_state & UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
    {
        // Validation of the background data failed.
        const wxString invalid_str = _(L("Invalid data"));
        for (auto btn : {ActionButtonType::abReslice, ActionButtonType::abSendGCode, ActionButtonType::abExport})
            sidebar->set_btn_label(btn, invalid_str);
    }
    else
    {
        // Background data is valid.
        if ((return_state & UPDATE_BACKGROUND_PROCESS_RESTART) != 0 ||
            (return_state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE) != 0 )
            this->statusbar()->set_status_text(_(L("Ready to slice")));

        sidebar->set_btn_label(ActionButtonType::abExport, _(label_btn_export));
        sidebar->set_btn_label(ActionButtonType::abSendGCode, _(label_btn_send));

        const wxString slice_string = background_process.running() && wxGetApp().get_mode() == comSimple ?
                                      _(L("Slicing")) + dots : _(L("Slice now"));
        sidebar->set_btn_label(ActionButtonType::abReslice, slice_string);

        if (background_process.finished())
            show_action_buttons(false);
        else if (!background_process.empty() &&
                 !background_process.running()) /* Do not update buttons if background process is running
                                                 * This condition is important for SLA mode especially,
                                                 * when this function is called several times during calculations
                                                 * */
            show_action_buttons(true);
    }

    return return_state;
}

// Restart background processing thread based on a bitmask of UpdateBackgroundProcessReturnState.
bool Plater::priv::restart_background_process(unsigned int state)
{
    if (m_ui_jobs.is_any_running()) {
        // Avoid a race condition
        return false;
    }

    if ( ! this->background_process.empty() &&
         (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID) == 0 &&
         ( ((state & UPDATE_BACKGROUND_PROCESS_FORCE_RESTART) != 0 && ! this->background_process.finished()) ||
           (state & UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT) != 0 ||
           (state & UPDATE_BACKGROUND_PROCESS_RESTART) != 0 ) ) {
        // The print is valid and it can be started.
        if (this->background_process.start()) {
            this->statusbar()->set_cancel_callback([this]() {
                this->statusbar()->set_status_text(_(L("Cancelling")));
                this->background_process.stop();
            });
            return true;
        }
    }
    return false;
}

void Plater::priv::export_gcode(fs::path output_path, bool output_path_on_removable_media, PrintHostJob upload_job)
{
    wxCHECK_RET(!(output_path.empty() && upload_job.empty()), "export_gcode: output_path and upload_job empty");

    if (model.objects.empty())
        return;

    if (background_process.is_export_scheduled()) {
        GUI::show_error(q, _(L("Another export job is currently running.")));
        return;
    }

    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = update_background_process(true);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        view3D->reload_scene(false);

    if ((state & priv::UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
        return;

    if (! output_path.empty()) {
        background_process.schedule_export(output_path.string(), output_path_on_removable_media);
    } else {
        background_process.schedule_upload(std::move(upload_job));
    }

    // If the SLA processing of just a single object's supports is running, restart slicing for the whole object.
    this->background_process.set_task(PrintBase::TaskParams());
    this->restart_background_process(priv::UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT);
}

unsigned int Plater::priv::update_restart_background_process(bool force_update_scene, bool force_update_preview)
{
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->update_background_process(false);
    if (force_update_scene || (state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE) != 0)
        view3D->reload_scene(false);

    if (force_update_preview)
        this->preview->reload_print();
    this->restart_background_process(state);
    return state;
}

void Plater::priv::update_fff_scene()
{
    if (this->preview != nullptr)
        this->preview->reload_print();
    // In case this was MM print, wipe tower bounding box on 3D tab might need redrawing with exact depth:
    view3D->reload_scene(true);
	
}

void Plater::priv::update_sla_scene()
{
    // Update the SLAPrint from the current Model, so that the reload_scene()
    // pulls the correct data.
    delayed_scene_refresh = false;
    this->update_restart_background_process(true, true);
}

void Plater::priv::reload_from_disk()
{
    Plater::TakeSnapshot snapshot(q, _(L("Reload from disk")));

    const Selection& selection = get_selection();

    if (selection.is_wipe_tower())
        return;

    // struct to hold selected ModelVolumes by their indices
    struct SelectedVolume
    {
        int object_idx;
        int volume_idx;

        // operators needed by std::algorithms
        bool operator < (const SelectedVolume& other) const { return (object_idx < other.object_idx) || ((object_idx == other.object_idx) && (volume_idx < other.volume_idx)); }
        bool operator == (const SelectedVolume& other) const { return (object_idx == other.object_idx) && (volume_idx == other.volume_idx); }
    };
    std::vector<SelectedVolume> selected_volumes;

    // collects selected ModelVolumes
    const std::set<unsigned int>& selected_volumes_idxs = selection.get_volume_idxs();
    for (unsigned int idx : selected_volumes_idxs)
    {
        const GLVolume* v = selection.get_volume(idx);
        int v_idx = v->volume_idx();
        if (v_idx >= 0)
        {
            int o_idx = v->object_idx();
            if ((0 <= o_idx) && (o_idx < (int)model.objects.size()))
                selected_volumes.push_back({ o_idx, v_idx });
        }
    }
    std::sort(selected_volumes.begin(), selected_volumes.end());
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end()), selected_volumes.end());

    // collects paths of files to load
    std::vector<fs::path> input_paths;
    std::vector<fs::path> missing_input_paths;
    for (const SelectedVolume& v : selected_volumes)
    {
        const ModelObject* object = model.objects[v.object_idx];
        const ModelVolume* volume = object->volumes[v.volume_idx];

        if (!volume->source.input_file.empty())
        {
            if (fs::exists(volume->source.input_file))
                input_paths.push_back(volume->source.input_file);
            else
                missing_input_paths.push_back(volume->source.input_file);
        }
        else if (!object->input_file.empty() && volume->is_model_part() && !volume->name.empty())
            missing_input_paths.push_back(volume->name);
    }

    std::sort(missing_input_paths.begin(), missing_input_paths.end());
    missing_input_paths.erase(std::unique(missing_input_paths.begin(), missing_input_paths.end()), missing_input_paths.end());

    while (!missing_input_paths.empty())
    {
        // ask user to select the missing file
        fs::path search = missing_input_paths.back();
        wxString title = _(L("Please select the file to reload"));
#if defined(__APPLE__)
        title += " (" + from_u8(search.filename().string()) + ")";
#endif // __APPLE__
        title += ":";
        wxFileDialog dialog(q, title, "", from_u8(search.filename().string()), file_wildcards(FT_MODEL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
            return;

        std::string sel_filename_path = dialog.GetPath().ToUTF8().data();
        std::string sel_filename = fs::path(sel_filename_path).filename().string();
        if (boost::algorithm::iequals(search.filename().string(), sel_filename))
        {
            input_paths.push_back(sel_filename_path);
            missing_input_paths.pop_back();

            fs::path sel_path = fs::path(sel_filename_path).remove_filename().string();

            std::vector<fs::path>::iterator it = missing_input_paths.begin();
            while (it != missing_input_paths.end())
            {
                // try to use the path of the selected file with all remaining missing files
                fs::path repathed_filename = sel_path;
                repathed_filename /= it->filename();
                if (fs::exists(repathed_filename))
                {
                    input_paths.push_back(repathed_filename.string());
                    it = missing_input_paths.erase(it);
                }
                else
                    ++it;
            }
        }
        else
        {
            wxString message = _(L("It is not allowed to change the file to reload")) + " (" + from_u8(search.filename().string()) + ").\n" + _(L("Do you want to retry")) + " ?";
            wxMessageDialog dlg(q, message, wxMessageBoxCaptionStr, wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);
            if (dlg.ShowModal() != wxID_YES)
                return;
        }
    }

    std::sort(input_paths.begin(), input_paths.end());
    input_paths.erase(std::unique(input_paths.begin(), input_paths.end()), input_paths.end());

    std::vector<wxString> fail_list;

    // load one file at a time
    for (size_t i = 0; i < input_paths.size(); ++i)
    {
        const auto& path = input_paths[i].string();

        wxBusyCursor wait;
        wxBusyInfo info(_(L("Reload from:")) + " " + from_u8(path), q->get_current_canvas3D()->get_wxglcanvas());

        Model new_model;
        try
        {
            new_model = Model::read_from_file(path, nullptr, true, false);
            for (ModelObject* model_object : new_model.objects)
            {
                model_object->center_around_origin();
                model_object->ensure_on_bed();
            }
        }
        catch (std::exception&)
        {
            // error while loading
            return;
        }

        // update the selected volumes whose source is the current file
        for (const SelectedVolume& sel_v : selected_volumes)
        {
            ModelObject* old_model_object = model.objects[sel_v.object_idx];
            ModelVolume* old_volume = old_model_object->volumes[sel_v.volume_idx];

            bool has_source = !old_volume->source.input_file.empty() && boost::algorithm::iequals(fs::path(old_volume->source.input_file).filename().string(), fs::path(path).filename().string());
            bool has_name = !old_volume->name.empty() && boost::algorithm::iequals(old_volume->name, fs::path(path).filename().string());
            if (has_source || has_name)
            {
                int new_volume_idx = -1;
                int new_object_idx = -1;
                if (has_source)
                {
                    // take idxs from source
                    new_volume_idx = old_volume->source.volume_idx;
                    new_object_idx = old_volume->source.object_idx;
                }
                else
                {
                    // take idxs from the 1st matching volume
                    for (size_t o = 0; o < new_model.objects.size(); ++o)
                    {
                        ModelObject* obj = new_model.objects[o];
                        bool found = false;
                        for (size_t v = 0; v < obj->volumes.size(); ++v)
                        {
                            if (obj->volumes[v]->name == old_volume->name)
                            {
                                new_volume_idx = (int)v;
                                new_object_idx = (int)o;
                                found = true;
                                break;
                            }
                        }
                        if (found)
                            break;
                    }
                }

                if ((new_object_idx < 0) && ((int)new_model.objects.size() <= new_object_idx))
                {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }
                ModelObject* new_model_object = new_model.objects[new_object_idx];
                if ((new_volume_idx < 0) && ((int)new_model.objects.size() <= new_volume_idx))
                {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }
                if (new_volume_idx < (int)new_model_object->volumes.size())
                {
                    old_model_object->add_volume(*new_model_object->volumes[new_volume_idx]);
                    ModelVolume* new_volume = old_model_object->volumes.back();
                    new_volume->set_new_unique_id();
                    new_volume->config.apply(old_volume->config);
                    new_volume->set_type(old_volume->type());
                    new_volume->set_material_id(old_volume->material_id());
                    new_volume->set_transformation(old_volume->get_transformation() * old_volume->source.transform);
                    new_volume->translate(new_volume->get_transformation().get_matrix(true) * (new_volume->source.mesh_offset - old_volume->source.mesh_offset));
                    std::swap(old_model_object->volumes[sel_v.volume_idx], old_model_object->volumes.back());
                    old_model_object->delete_volume(old_model_object->volumes.size() - 1);
                    old_model_object->ensure_on_bed();
                }
            }
        }
    }

    if (!fail_list.empty())
    {
        wxString message = _(L("Unable to reload:")) + "\n";
        for (const wxString& s : fail_list)
        {
            message += s + "\n";
        }
        wxMessageDialog dlg(q, message, _(L("Error during reload")), wxOK | wxOK_DEFAULT | wxICON_WARNING);
        dlg.ShowModal();
    }

    // update 3D scene
    update();

    // new GLVolumes have been created at this point, so update their printable state
    for (size_t i = 0; i < model.objects.size(); ++i)
    {
        view3D->get_canvas3d()->update_instance_printable_state_for_object(i);
    }
}

void Plater::priv::reload_all_from_disk()
{
    if (model.objects.empty())
        return;

    Plater::TakeSnapshot snapshot(q, _(L("Reload all from disk")));
    Plater::SuppressSnapshots suppress(q);

    Selection& selection = get_selection();
    Selection::IndicesList curr_idxs = selection.get_volume_idxs();
    // reload from disk uses selection
    select_all();
    reload_from_disk();
    // restore previous selection
    selection.clear();
    for (unsigned int idx : curr_idxs)
    {
        selection.add(idx, false);
    }
}

void Plater::priv::fix_through_netfabb(const int obj_idx, const int vol_idx/* = -1*/)
{
    if (obj_idx < 0)
        return;

    Plater::TakeSnapshot snapshot(q, _(L("Fix Throught NetFabb")));

    fix_model_by_win10_sdk_gui(*model.objects[obj_idx], vol_idx);
    this->update();
    this->object_list_changed();
    this->schedule_background_process();
}

void Plater::priv::set_current_panel(wxPanel* panel)
{
    if (std::find(panels.begin(), panels.end(), panel) == panels.end())
        return;

#ifdef __WXMAC__
    bool force_render = (current_panel != nullptr);
#endif // __WXMAC__

    if (current_panel == panel)
        return;

    current_panel = panel;
    // to reduce flickering when changing view, first set as visible the new current panel
    for (wxPanel* p : panels)
    {
        if (p == current_panel)
        {
#ifdef __WXMAC__
            // On Mac we need also to force a render to avoid flickering when changing view
            if (force_render)
            {
                if (p == view3D)
                    dynamic_cast<View3D*>(p)->get_canvas3d()->render();
                else if (p == preview)
                    dynamic_cast<Preview*>(p)->get_canvas3d()->render();
            }
#endif // __WXMAC__
            p->Show();
        }
    }
    // then set to invisible the other
    for (wxPanel* p : panels)
    {
        if (p != current_panel)
            p->Hide();
    }

    panel_sizer->Layout();

    if (current_panel == view3D)
    {
        if (view3D->is_reload_delayed())
        {
            // Delayed loading of the 3D scene.
            if (this->printer_technology == ptSLA)
            {
                // Update the SLAPrint from the current Model, so that the reload_scene()
                // pulls the correct data.
                this->update_restart_background_process(true, false);
            } else
                view3D->reload_scene(true);
        }

        // sets the canvas as dirty to force a render at the 1st idle event (wxWidgets IsShownOnScreen() is buggy and cannot be used reliably)
        view3D->set_as_dirty();
        view_toolbar.select_item("3D");
    }
    else if (current_panel == preview)
    {
        // see: Plater::priv::object_list_changed()
        // FIXME: it may be better to have a single function making this check and let it be called wherever needed
        bool export_in_progress = this->background_process.is_export_scheduled();
        bool model_fits = view3D->check_volumes_outside_state() != ModelInstance::PVS_Partly_Outside;
        if (!model.objects.empty() && !export_in_progress && model_fits)
            this->q->reslice();
        // keeps current gcode preview, if any
        preview->reload_print(true);

        preview->set_as_dirty();
        view_toolbar.select_item("Preview");
    }

    current_panel->SetFocusFromKbd();
}

void Plater::priv::on_select_preset(wxCommandEvent &evt)
{
    auto preset_type = static_cast<Preset::Type>(evt.GetInt());
    auto *combo = static_cast<PresetComboBox*>(evt.GetEventObject());

    // see https://github.com/prusa3d/PrusaSlicer/issues/3889
    // Under OSX: in case of use of a same names written in different case (like "ENDER" and "Ender"),
    // m_presets_choice->GetSelection() will return first item, because search in PopupListCtrl is case-insensitive.
    // So, use GetSelection() from event parameter 
    // But in this function we couldn't use evt.GetSelection(), because m_commandInt is used for preset_type
    // Thus, get selection in this way:
    int selection = combo->FindString(evt.GetString(), true);

    auto idx = combo->get_extruder_idx();

    //! Because of The MSW and GTK version of wxBitmapComboBox derived from wxComboBox,
    //! but the OSX version derived from wxOwnerDrawnCombo.
    //! So, to get selected string we do
    //!     combo->GetString(combo->GetSelection())
    //! instead of
    //!     combo->GetStringSelection().ToUTF8().data());

    const std::string preset_name = wxGetApp().preset_bundle->get_preset_name_by_alias(preset_type, 
        Preset::remove_suffix_modified(combo->GetString(selection).ToUTF8().data()));

    if (preset_type == Preset::TYPE_FILAMENT) {
        wxGetApp().preset_bundle->set_filament_preset(idx, preset_name);
    }

    // TODO: ?
    if (preset_type == Preset::TYPE_FILAMENT && sidebar->is_multifilament()) {
        // Only update the plater UI for the 2nd and other filaments.
        wxGetApp().preset_bundle->update_plater_filament_ui(idx, combo);
    }
    else {
        wxWindowUpdateLocker noUpdates(sidebar->presets_panel());
        wxGetApp().get_tab(preset_type)->select_preset(preset_name);
    }

    // update plater with new config
    wxGetApp().plater()->on_config_change(wxGetApp().preset_bundle->full_config());
    /* Settings list can be changed after printer preset changing, so
     * update all settings items for all item had it.
     * Furthermore, Layers editing is implemented only for FFF printers
     * and for SLA presets they should be deleted
     */
    if (preset_type == Preset::TYPE_PRINTER)
        wxGetApp().obj_list()->update_object_list_by_printer_technology();
}

void Plater::priv::on_slicing_update(SlicingStatusEvent &evt)
{
    if (evt.status.percent >= -1) {
        if (m_ui_jobs.is_any_running()) {
            // Avoid a race condition
            return;
        }

        this->statusbar()->set_progress(evt.status.percent);
        this->statusbar()->set_status_text(_(evt.status.text) + wxString::FromUTF8("…"));
    }
    if (evt.status.flags & (PrintBase::SlicingStatus::RELOAD_SCENE | PrintBase::SlicingStatus::RELOAD_SLA_SUPPORT_POINTS)) {
        switch (this->printer_technology) {
        case ptFFF:
            this->update_fff_scene();
            break;
        case ptSLA:
            // If RELOAD_SLA_SUPPORT_POINTS, then the SLA gizmo is updated (reload_scene calls update_gizmos_data)
            if (view3D->is_dragging())
                delayed_scene_refresh = true;
            else
                this->update_sla_scene();
            break;
        default: break;
        }
    } else if (evt.status.flags & PrintBase::SlicingStatus::RELOAD_SLA_PREVIEW) {
        // Update the SLA preview. Only called if not RELOAD_SLA_SUPPORT_POINTS, as the block above will refresh the preview anyways.
        this->preview->reload_print();
    }
}

void Plater::priv::on_slicing_completed(wxCommandEvent &)
{
    switch (this->printer_technology) {
    case ptFFF:
        this->update_fff_scene();
        break;
    case ptSLA:
        if (view3D->is_dragging())
            delayed_scene_refresh = true;
        else
            this->update_sla_scene();
        break;
    default: break;
    }
}

void Plater::priv::on_process_completed(wxCommandEvent &evt)
{
    // Stop the background task, wait until the thread goes into the "Idle" state.
    // At this point of time the thread should be either finished or canceled,
    // so the following call just confirms, that the produced data were consumed.
    this->background_process.stop();
    this->statusbar()->reset_cancel_callback();
    this->statusbar()->stop_busy();

    const bool canceled = evt.GetInt() < 0;
    const bool error = evt.GetInt() == 0;
    const bool success  = evt.GetInt() > 0;
    // Reset the "export G-code path" name, so that the automatic background processing will be enabled again.
    this->background_process.reset_export();

    if (error) {
        wxString message = evt.GetString();
        if (message.IsEmpty())
            message = _(L("Export failed"));
        if (q->m_tracking_popup_menu)
        	// We don't want to pop-up a message box when tracking a pop-up menu.
        	// We postpone the error message instead.
            q->m_tracking_popup_menu_error_message = message;
        else
	        show_error(q, message);
        this->statusbar()->set_status_text(message);
    }
    if (canceled)
        this->statusbar()->set_status_text(_(L("Cancelled")));

    this->sidebar->show_sliced_info_sizer(success);

    // This updates the "Slice now", "Export G-code", "Arrange" buttons status.
    // Namely, it refreshes the "Out of print bed" property of all the ModelObjects, and it enables
    // the "Slice now" and "Export G-code" buttons based on their "out of bed" status.
    this->object_list_changed();

    // refresh preview
    switch (this->printer_technology) {
    case ptFFF:
        this->update_fff_scene();
        break;
    case ptSLA:
        if (view3D->is_dragging())
            delayed_scene_refresh = true;
        else
            this->update_sla_scene();
        break;
    default: break;
    }
	

    if (canceled) {
        if (wxGetApp().get_mode() == comSimple)
            sidebar->set_btn_label(ActionButtonType::abReslice, "Slice now");
        show_action_buttons(true);
    }
    else if (this->writing_to_removable_device || wxGetApp().get_mode() == comSimple)
		show_action_buttons(false);
    this->writing_to_removable_device = false;
}

void Plater::priv::on_layer_editing_toggled(bool enable)
{
    view3D->enable_layers_editing(enable);
    view3D->set_as_dirty();
}

void Plater::priv::on_action_add(SimpleEvent&)
{
    if (q != nullptr)
        q->add_model();
}

void Plater::priv::on_action_split_objects(SimpleEvent&)
{
    split_object();
}

void Plater::priv::on_action_split_volumes(SimpleEvent&)
{
    split_volume();
}

void Plater::priv::on_action_layersediting(SimpleEvent&)
{
    view3D->enable_layers_editing(!view3D->is_layers_editing_enabled());
}

void Plater::priv::on_object_select(SimpleEvent& evt)
{
//    this->take_snapshot(_(L("Object Selection")));

    wxGetApp().obj_list()->update_selections();
    selection_changed();
}

void Plater::priv::on_right_click(RBtnEvent& evt)
{
    int obj_idx = get_selected_object_idx();

    wxMenu* menu = nullptr;

    if (obj_idx == -1) // no one or several object are selected
    { 
        if (evt.data.second) // right button was clicked on empty space
            menu = &default_menu;
        else
        {
            sidebar->obj_list()->show_multi_selection_menu();
            return;
        }
    }
    else
    {
        // If in 3DScene is(are) selected volume(s), but right button was clicked on empty space
        if (evt.data.second)
            return; 

        if (printer_technology == ptSLA)
            menu = &sla_object_menu;
        else
        {
            // show "Object menu" for each one or several FullInstance instead of FullObject
            const bool is_some_full_instances = get_selection().is_single_full_instance() || 
                                                get_selection().is_single_full_object() || 
                                                get_selection().is_multiple_full_instance();
            menu = is_some_full_instances ? &object_menu : &part_menu;
        }

        sidebar->obj_list()->append_menu_item_settings(menu);

        if (printer_technology != ptSLA)
            sidebar->obj_list()->append_menu_item_change_extruder(menu);

        if (menu != &part_menu)
        {
            /* Remove/Prepend "increase/decrease instances" menu items according to the view mode.
             * Suppress to show those items for a Simple mode
             */
            const MenuIdentifier id = printer_technology == ptSLA ? miObjectSLA : miObjectFFF;
            if (wxGetApp().get_mode() == comSimple) {
                if (menu->FindItem(_(L("Add instance"))) != wxNOT_FOUND)
                {
                    /* Detach an items from the menu, but don't delete them
                     * so that they can be added back later
                     * (after switching to the Advanced/Expert mode)
                     */
                    menu->Remove(items_increase[id]);
                    menu->Remove(items_decrease[id]);
                    menu->Remove(items_set_number_of_copies[id]);
                }
            }
            else {
                if (menu->FindItem(_(L("Add instance"))) == wxNOT_FOUND)
                {
                    // Prepend items to the menu, if those aren't not there
                    menu->Prepend(items_set_number_of_copies[id]);
                    menu->Prepend(items_decrease[id]);
                    menu->Prepend(items_increase[id]);
                }
            }
        }
    }

    if (q != nullptr && menu) {
#ifdef __linux__
        // For some reason on Linux the menu isn't displayed if position is specified
        // (even though the position is sane).
        q->PopupMenu(menu);
#else
        q->PopupMenu(menu, (int)evt.data.first.x(), (int)evt.data.first.y());
#endif
    }
}

void Plater::priv::on_wipetower_moved(Vec3dEvent &evt)
{
    DynamicPrintConfig cfg;
    cfg.opt<ConfigOptionFloat>("wipe_tower_x", true)->value = evt.data(0);
    cfg.opt<ConfigOptionFloat>("wipe_tower_y", true)->value = evt.data(1);
    wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(cfg);
}

void Plater::priv::on_wipetower_rotated(Vec3dEvent& evt)
{
    DynamicPrintConfig cfg;
    cfg.opt<ConfigOptionFloat>("wipe_tower_x", true)->value = evt.data(0);
    cfg.opt<ConfigOptionFloat>("wipe_tower_y", true)->value = evt.data(1);
    cfg.opt<ConfigOptionFloat>("wipe_tower_rotation_angle", true)->value = Geometry::rad2deg(evt.data(2));
    wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(cfg);
}

void Plater::priv::on_update_geometry(Vec3dsEvent<2>&)
{
    // TODO
}

// Update the scene from the background processing,
// if the update message was received during mouse manipulation.
void Plater::priv::on_3dcanvas_mouse_dragging_finished(SimpleEvent&)
{
    if (this->delayed_scene_refresh) {
        this->delayed_scene_refresh = false;
        this->update_sla_scene();
    }
}

bool Plater::priv::init_object_menu()
{
    items_increase.reserve(2);
    items_decrease.reserve(2);
    items_set_number_of_copies.reserve(2);

    init_common_menu(&object_menu);
    complit_init_object_menu();

    init_common_menu(&sla_object_menu);
    complit_init_sla_object_menu();

    init_common_menu(&part_menu, true);
    complit_init_part_menu();

    sidebar->obj_list()->create_default_popupmenu(&default_menu);

    return true;
}

void Plater::priv::generate_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, bool printable_only, bool parts_only, bool show_bed, bool transparent_background)
{
    view3D->get_canvas3d()->render_thumbnail(data, w, h, printable_only, parts_only, show_bed, transparent_background);
}

void Plater::priv::generate_thumbnails(ThumbnailsList& thumbnails, const Vec2ds& sizes, bool printable_only, bool parts_only, bool show_bed, bool transparent_background)
{
    thumbnails.clear();
    for (const Vec2d& size : sizes)
    {
        thumbnails.push_back(ThumbnailData());
        Point isize(size); // round to ints
        generate_thumbnail(thumbnails.back(), isize.x(), isize.y(), printable_only, parts_only, show_bed, transparent_background);
        if (!thumbnails.back().is_valid())
            thumbnails.pop_back();
    }
}

void Plater::priv::msw_rescale_object_menu()
{
    for (MenuWithSeparators* menu : { &object_menu, &sla_object_menu, &part_menu, &default_menu })
        msw_rescale_menu(dynamic_cast<wxMenu*>(menu));
}

wxString Plater::priv::get_project_filename(const wxString& extension) const
{
    return m_project_filename.empty() ? "" : m_project_filename + extension;
}

void Plater::priv::set_project_filename(const wxString& filename)
{
    boost::filesystem::path full_path = into_path(filename);
    boost::filesystem::path ext = full_path.extension();
    if (boost::iequals(ext.string(), ".amf")) {
        // Remove the first extension.
        full_path.replace_extension("");
        // It may be ".zip.amf".
        if (boost::iequals(full_path.extension().string(), ".zip"))
            // Remove the 2nd extension.
            full_path.replace_extension("");
    } else {
        // Remove just one extension.
        full_path.replace_extension("");
    }

    m_project_filename = from_path(full_path);
    wxGetApp().mainframe->update_title();

    if (!filename.empty())
        wxGetApp().mainframe->add_to_recent_projects(filename);
}

bool Plater::priv::init_common_menu(wxMenu* menu, const bool is_part/* = false*/)
{
    if (is_part) {
        append_menu_item(menu, wxID_ANY, _(L("Delete")) + "\tDel", _(L("Remove the selected object")),
            [this](wxCommandEvent&) { q->remove_selected();         }, "delete",            nullptr, [this]() { return can_delete(); }, q);

        append_menu_item(menu, wxID_ANY, _(L("Reload from disk")), _(L("Reload the selected volumes from disk")),
            [this](wxCommandEvent&) { q->reload_from_disk(); }, "", menu, [this]() { return can_reload_from_disk(); }, q);

        sidebar->obj_list()->append_menu_item_export_stl(menu);
    }
    else {
        wxMenuItem* item_increase = append_menu_item(menu, wxID_ANY, _(L("Add instance")) + "\t+", _(L("Add one more instance of the selected object")),
            [this](wxCommandEvent&) { q->increase_instances();      }, "add_copies",        nullptr, [this]() { return can_increase_instances(); }, q);
        wxMenuItem* item_decrease = append_menu_item(menu, wxID_ANY, _(L("Remove instance")) + "\t-", _(L("Remove one instance of the selected object")),
            [this](wxCommandEvent&) { q->decrease_instances();      }, "remove_copies",     nullptr, [this]() { return can_decrease_instances(); }, q);
        wxMenuItem* item_set_number_of_copies = append_menu_item(menu, wxID_ANY, _(L("Set number of instances")) + dots, _(L("Change the number of instances of the selected object")),
            [this](wxCommandEvent&) { q->set_number_of_copies();    }, "number_of_copies",  nullptr, [this]() { return can_increase_instances(); }, q);


        items_increase.push_back(item_increase);
        items_decrease.push_back(item_decrease);
        items_set_number_of_copies.push_back(item_set_number_of_copies);

        // Delete menu was moved to be after +/- instace to make it more difficult to be selected by mistake.
        append_menu_item(menu, wxID_ANY, _(L("Delete")) + "\tDel", _(L("Remove the selected object")),
            [this](wxCommandEvent&) { q->remove_selected(); }, "delete",            nullptr, [this]() { return can_delete(); }, q);

        menu->AppendSeparator();
        sidebar->obj_list()->append_menu_item_instance_to_object(menu, q);
        menu->AppendSeparator();

        wxMenuItem* menu_item_printable = sidebar->obj_list()->append_menu_item_printable(menu, q);
        menu->AppendSeparator();

        append_menu_item(menu, wxID_ANY, _(L("Reload from disk")), _(L("Reload the selected object from disk")),
            [this](wxCommandEvent&) { reload_from_disk(); }, "", nullptr, [this]() { return can_reload_from_disk(); }, q);

        append_menu_item(menu, wxID_ANY, _(L("Export as STL")) + dots, _(L("Export the selected object as STL file")),
            [this](wxCommandEvent&) { q->export_stl(false, true); }, "", nullptr, 
            [this]() {
                const Selection& selection = get_selection();
                return selection.is_single_full_instance() || selection.is_single_full_object();
            }, q);

        menu->AppendSeparator();

        q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) {
            const Selection& selection = get_selection();
            int instance_idx = selection.get_instance_idx();
            evt.Enable(selection.is_single_full_instance() || selection.is_single_full_object());
            if (instance_idx != -1)
            {
                evt.Check(model.objects[selection.get_object_idx()]->instances[instance_idx]->printable);
                view3D->set_as_dirty();
            }
            }, menu_item_printable->GetId());
    }

    sidebar->obj_list()->append_menu_item_fix_through_netfabb(menu);

    sidebar->obj_list()->append_menu_item_scale_selection_to_fit_print_volume(menu);

    wxMenu* mirror_menu = new wxMenu();
    if (mirror_menu == nullptr)
        return false;

    append_menu_item(mirror_menu, wxID_ANY, _(L("Along X axis")), _(L("Mirror the selected object along the X axis")),
        [this](wxCommandEvent&) { mirror(X); }, "mark_X", menu);
    append_menu_item(mirror_menu, wxID_ANY, _(L("Along Y axis")), _(L("Mirror the selected object along the Y axis")),
        [this](wxCommandEvent&) { mirror(Y); }, "mark_Y", menu);
    append_menu_item(mirror_menu, wxID_ANY, _(L("Along Z axis")), _(L("Mirror the selected object along the Z axis")),
        [this](wxCommandEvent&) { mirror(Z); }, "mark_Z", menu);

    append_submenu(menu, mirror_menu, wxID_ANY, _(L("Mirror")), _(L("Mirror the selected object")), "",
        [this]() { return can_mirror(); }, q);

    return true;
}

bool Plater::priv::complit_init_object_menu()
{
    wxMenu* split_menu = new wxMenu();
    if (split_menu == nullptr)
        return false;

    append_menu_item(split_menu, wxID_ANY, _(L("To objects")), _(L("Split the selected object into individual objects")),
        [this](wxCommandEvent&) { split_object(); }, "split_object_SMALL",  &object_menu, [this]() { return can_split(); }, q);
    append_menu_item(split_menu, wxID_ANY, _(L("To parts")), _(L("Split the selected object into individual sub-parts")),
        [this](wxCommandEvent&) { split_volume(); }, "split_parts_SMALL",   &object_menu, [this]() { return can_split(); }, q);

    append_submenu(&object_menu, split_menu, wxID_ANY, _(L("Split")), _(L("Split the selected object")), "",
        [this]() { return can_split() && wxGetApp().get_mode() > comSimple; }, q);
    object_menu.AppendSeparator();

    // Layers Editing for object
    sidebar->obj_list()->append_menu_item_layers_editing(&object_menu, q);
    object_menu.AppendSeparator();

    // "Add (volumes)" popupmenu will be added later in append_menu_items_add_volume()

    return true;
}

bool Plater::priv::complit_init_sla_object_menu()
{
    append_menu_item(&sla_object_menu, wxID_ANY, _(L("Split")), _(L("Split the selected object into individual objects")),
        [this](wxCommandEvent&) { split_object(); }, "split_object_SMALL", nullptr, [this]() { return can_split(); }, q);

    sla_object_menu.AppendSeparator();

    // Add the automatic rotation sub-menu
    append_menu_item(&sla_object_menu, wxID_ANY, _(L("Optimize orientation")), _(L("Optimize the rotation of the object for better print results.")),
        [this](wxCommandEvent&) { sla_optimize_rotation(); });

    return true;
}

bool Plater::priv::complit_init_part_menu()
{
    append_menu_item(&part_menu, wxID_ANY, _(L("Split")), _(L("Split the selected object into individual sub-parts")),
        [this](wxCommandEvent&) { split_volume(); }, "split_parts_SMALL", nullptr, [this]() { return can_split(); }, q);

    part_menu.AppendSeparator();

    auto obj_list = sidebar->obj_list();
    obj_list->append_menu_item_change_type(&part_menu, q);

    return true;
}

void Plater::priv::set_current_canvas_as_dirty()
{
    if (current_panel == view3D)
        view3D->set_as_dirty();
    else if (current_panel == preview)
        preview->set_as_dirty();
}

GLCanvas3D* Plater::priv::get_current_canvas3D()
{
    return (current_panel == view3D) ? view3D->get_canvas3d() : ((current_panel == preview) ? preview->get_canvas3d() : nullptr);
}

bool Plater::priv::init_view_toolbar()
{
    if (view_toolbar.get_items_count() > 0)
        // already initialized
        return true;

    BackgroundTexture::Metadata background_data;
    background_data.filename = "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (!view_toolbar.init(background_data))
        return false;

    view_toolbar.set_horizontal_orientation(GLToolbar::Layout::HO_Left);
    view_toolbar.set_vertical_orientation(GLToolbar::Layout::VO_Bottom);
    view_toolbar.set_border(5.0f);
    view_toolbar.set_gap_size(1.0f);

    GLToolbarItem::Data item;

    item.name = "3D";
    item.icon_filename = "editor.svg";
    item.tooltip = _utf8(L("3D editor view")) + " [" + GUI::shortkey_ctrl_prefix() + "5]";
    item.sprite_id = 0;
    item.left.action_callback = [this]() { if (this->q != nullptr) wxPostEvent(this->q, SimpleEvent(EVT_GLVIEWTOOLBAR_3D)); };
    if (!view_toolbar.add_item(item))
        return false;

    item.name = "Preview";
    item.icon_filename = "preview.svg";
    item.tooltip = _utf8(L("Preview")) + " [" + GUI::shortkey_ctrl_prefix() + "6]";
    item.sprite_id = 1;
    item.left.action_callback = [this]() { if (this->q != nullptr) wxPostEvent(this->q, SimpleEvent(EVT_GLVIEWTOOLBAR_PREVIEW)); };
    if (!view_toolbar.add_item(item))
        return false;

    view_toolbar.select_item("3D");
    view_toolbar.set_enabled(true);

    return true;
}

bool Plater::priv::can_set_instance_to_object() const
{
    const int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size()) && (model.objects[obj_idx]->instances.size() > 1);
}

bool Plater::priv::can_split() const
{
    return sidebar->obj_list()->is_splittable();
}

bool Plater::priv::layers_height_allowed() const
{
    if (printer_technology != ptFFF)
        return false;

    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size()) && config->opt_bool("variable_layer_height") && view3D->is_layers_editing_allowed();
}

bool Plater::priv::can_mirror() const
{
    return get_selection().is_from_single_instance();
}

bool Plater::priv::can_reload_from_disk() const
{
    // struct to hold selected ModelVolumes by their indices
    struct SelectedVolume
    {
        int object_idx;
        int volume_idx;

        // operators needed by std::algorithms
        bool operator < (const SelectedVolume& other) const { return (object_idx < other.object_idx) || ((object_idx == other.object_idx) && (volume_idx < other.volume_idx)); }
        bool operator == (const SelectedVolume& other) const { return (object_idx == other.object_idx) && (volume_idx == other.volume_idx); }
    };
    std::vector<SelectedVolume> selected_volumes;

    const Selection& selection = get_selection();

    // collects selected ModelVolumes
    const std::set<unsigned int>& selected_volumes_idxs = selection.get_volume_idxs();
    for (unsigned int idx : selected_volumes_idxs)
    {
        const GLVolume* v = selection.get_volume(idx);
        int v_idx = v->volume_idx();
        if (v_idx >= 0)
        {
            int o_idx = v->object_idx();
            if ((0 <= o_idx) && (o_idx < (int)model.objects.size()))
                selected_volumes.push_back({ o_idx, v_idx });
        }
    }
    std::sort(selected_volumes.begin(), selected_volumes.end());
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end()), selected_volumes.end());

    // collects paths of files to load
    std::vector<fs::path> paths;
    for (const SelectedVolume& v : selected_volumes)
    {
        const ModelObject* object = model.objects[v.object_idx];
        const ModelVolume* volume = object->volumes[v.volume_idx];
        if (!volume->source.input_file.empty())
            paths.push_back(volume->source.input_file);
        else if (!object->input_file.empty() && !volume->name.empty())
            paths.push_back(volume->name);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    return !paths.empty();
}

void Plater::priv::set_bed_shape(const Pointfs& shape, const std::string& custom_texture, const std::string& custom_model)
{
    bool new_shape = bed.set_shape(shape, custom_texture, custom_model);
    if (new_shape)
    {
        if (view3D) view3D->bed_shape_changed();
        if (preview) preview->bed_shape_changed();
    }
}

bool Plater::priv::can_delete() const
{
    return !get_selection().is_empty() && !get_selection().is_wipe_tower() && !m_ui_jobs.is_any_running();
}

bool Plater::priv::can_delete_all() const
{
    return !model.objects.empty();
}

bool Plater::priv::can_fix_through_netfabb() const
{
    int obj_idx = get_selected_object_idx();
    if (obj_idx < 0)
        return false;

    return model.objects[obj_idx]->get_mesh_errors_count() > 0;
}

bool Plater::priv::can_increase_instances() const
{
    if (m_ui_jobs.is_any_running()) {
        return false;
    }

    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size());
}

bool Plater::priv::can_decrease_instances() const
{
    if (m_ui_jobs.is_any_running()) {
        return false;
    }

    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size()) && (model.objects[obj_idx]->instances.size() > 1);
}

bool Plater::priv::can_split_to_objects() const
{
    return can_split();
}

bool Plater::priv::can_split_to_volumes() const
{
    return (printer_technology != ptSLA) && can_split();
}

bool Plater::priv::can_arrange() const
{
    return !model.objects.empty() && !m_ui_jobs.is_any_running();
}

bool Plater::priv::can_layers_editing() const
{
    return layers_height_allowed();
}

void Plater::priv::update_object_menu()
{
    sidebar->obj_list()->append_menu_items_add_volume(&object_menu);
}

void Plater::priv::show_action_buttons(const bool ready_to_slice) const
{
	// Cache this value, so that the callbacks from the RemovableDriveManager may repeat that value when calling show_action_buttons().
    this->ready_to_slice = ready_to_slice;

    wxWindowUpdateLocker noUpdater(sidebar);
    const auto prin_host_opt = config->option<ConfigOptionString>("print_host");
    const bool send_gcode_shown = prin_host_opt != nullptr && !prin_host_opt->value.empty();
    
    // when a background processing is ON, export_btn and/or send_btn are showing
    if (wxGetApp().app_config->get("background_processing") == "1")
    {
	    RemovableDriveManager::RemovableDrivesStatus removable_media_status = wxGetApp().removable_drive_manager()->status();
		if (sidebar->show_reslice(false) |
			sidebar->show_export(true) |
			sidebar->show_send(send_gcode_shown) |
			sidebar->show_export_removable(removable_media_status.has_removable_drives) |
			sidebar->show_disconnect(removable_media_status.has_eject))
            sidebar->Layout();
    }
    else
    {
	    RemovableDriveManager::RemovableDrivesStatus removable_media_status;
	    if (! ready_to_slice) 
	    	removable_media_status = wxGetApp().removable_drive_manager()->status();
        if (sidebar->show_reslice(ready_to_slice) |
            sidebar->show_export(!ready_to_slice) |
            sidebar->show_send(send_gcode_shown && !ready_to_slice) |
			sidebar->show_export_removable(!ready_to_slice && removable_media_status.has_removable_drives) |
            sidebar->show_disconnect(!ready_to_slice && removable_media_status.has_eject))
            sidebar->Layout();
    }
}

void Plater::priv::enter_gizmos_stack()
{
    assert(m_undo_redo_stack_active == &m_undo_redo_stack_main);
    if (m_undo_redo_stack_active == &m_undo_redo_stack_main) {
        m_undo_redo_stack_active = &m_undo_redo_stack_gizmos;
        assert(m_undo_redo_stack_active->empty());
        // Take the initial snapshot of the gizmos.
        // Not localized on purpose, the text will never be shown to the user.
        this->take_snapshot(std::string("Gizmos-Initial"));
    }
}

void Plater::priv::leave_gizmos_stack()
{
    assert(m_undo_redo_stack_active == &m_undo_redo_stack_gizmos);
    if (m_undo_redo_stack_active == &m_undo_redo_stack_gizmos) {
        assert(! m_undo_redo_stack_active->empty());
        m_undo_redo_stack_active->clear();
        m_undo_redo_stack_active = &m_undo_redo_stack_main;
    }
}

int Plater::priv::get_active_snapshot_index()
{
    const size_t active_snapshot_time = this->undo_redo_stack().active_snapshot_time();
    const std::vector<UndoRedo::Snapshot>& ss_stack = this->undo_redo_stack().snapshots();
    const auto it = std::lower_bound(ss_stack.begin(), ss_stack.end(), UndoRedo::Snapshot(active_snapshot_time));
    return it - ss_stack.begin();
}

void Plater::priv::take_snapshot(const std::string& snapshot_name)
{
    if (this->m_prevent_snapshots > 0)
        return;
    assert(this->m_prevent_snapshots >= 0);
    UndoRedo::SnapshotData snapshot_data;
    snapshot_data.printer_technology = this->printer_technology;
    if (this->view3D->is_layers_editing_enabled())
        snapshot_data.flags |= UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE;
    if (this->sidebar->obj_list()->is_selected(itSettings)) {
        snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR;
        snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayer)) {
        snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR;
        snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayerRoot))
        snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR;

    // If SLA gizmo is active, ask it if it wants to trigger support generation
    // on loading this snapshot.
    if (view3D->get_canvas3d()->get_gizmos_manager().wants_reslice_supports_on_undo())
        snapshot_data.flags |= UndoRedo::SnapshotData::RECALCULATE_SLA_SUPPORTS;

    //FIXME updating the Wipe tower config values at the ModelWipeTower from the Print config.
    // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
    if (this->printer_technology == ptFFF) {
        const DynamicPrintConfig &config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        model.wipe_tower.position = Vec2d(config.opt_float("wipe_tower_x"), config.opt_float("wipe_tower_y"));
        model.wipe_tower.rotation = config.opt_float("wipe_tower_rotation_angle");
    }
    this->undo_redo_stack().take_snapshot(snapshot_name, model, view3D->get_canvas3d()->get_selection(), view3D->get_canvas3d()->get_gizmos_manager(), snapshot_data);
    this->undo_redo_stack().release_least_recently_used();
    // Save the last active preset name of a particular printer technology.
    ((this->printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name) = wxGetApp().preset_bundle->printers.get_selected_preset_name();
    BOOST_LOG_TRIVIAL(info) << "Undo / Redo snapshot taken: " << snapshot_name << ", Undo / Redo stack memory: " << Slic3r::format_memsize_MB(this->undo_redo_stack().memsize()) << log_memory_info();
}

void Plater::priv::undo()
{
    const std::vector<UndoRedo::Snapshot> &snapshots = this->undo_redo_stack().snapshots();
    auto it_current = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(this->undo_redo_stack().active_snapshot_time()));
    if (-- it_current != snapshots.begin())
        this->undo_redo_to(it_current);
}

void Plater::priv::redo()
{
    const std::vector<UndoRedo::Snapshot> &snapshots = this->undo_redo_stack().snapshots();
    auto it_current = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(this->undo_redo_stack().active_snapshot_time()));
    if (++ it_current != snapshots.end())
        this->undo_redo_to(it_current);
}

void Plater::priv::undo_redo_to(size_t time_to_load)
{
    const std::vector<UndoRedo::Snapshot> &snapshots = this->undo_redo_stack().snapshots();
    auto it_current = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(time_to_load));
    assert(it_current != snapshots.end());
    this->undo_redo_to(it_current);
}

void Plater::priv::undo_redo_to(std::vector<UndoRedo::Snapshot>::const_iterator it_snapshot)
{
    // Make sure that no updating function calls take_snapshot until we are done.
    SuppressSnapshots snapshot_supressor(q);

    bool 				temp_snapshot_was_taken 	= this->undo_redo_stack().temp_snapshot_active();
    PrinterTechnology 	new_printer_technology 		= it_snapshot->snapshot_data.printer_technology;
    bool 				printer_technology_changed 	= this->printer_technology != new_printer_technology;
    if (printer_technology_changed) {
        // Switching the printer technology when jumping forwards / backwards in time. Switch to the last active printer profile of the other type.
        std::string s_pt = (it_snapshot->snapshot_data.printer_technology == ptFFF) ? "FFF" : "SLA";
        if (! wxGetApp().check_unsaved_changes(from_u8((boost::format(_utf8(
            L("%1% printer was active at the time the target Undo / Redo snapshot was taken. Switching to %1% printer requires reloading of %1% presets."))) % s_pt).str())))
            // Don't switch the profiles.
            return;
    }
    // Save the last active preset name of a particular printer technology.
    ((this->printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name) = wxGetApp().preset_bundle->printers.get_selected_preset_name();
    //FIXME updating the Wipe tower config values at the ModelWipeTower from the Print config.
    // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
    if (this->printer_technology == ptFFF) {
        const DynamicPrintConfig &config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
                model.wipe_tower.position = Vec2d(config.opt_float("wipe_tower_x"), config.opt_float("wipe_tower_y"));
                model.wipe_tower.rotation = config.opt_float("wipe_tower_rotation_angle");
    }
    const int layer_range_idx = it_snapshot->snapshot_data.layer_range_idx;
    // Flags made of Snapshot::Flags enum values.
    unsigned int new_flags = it_snapshot->snapshot_data.flags;
    UndoRedo::SnapshotData top_snapshot_data;
    top_snapshot_data.printer_technology = this->printer_technology;
    if (this->view3D->is_layers_editing_enabled())
        top_snapshot_data.flags |= UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE;
    if (this->sidebar->obj_list()->is_selected(itSettings)) {
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR;
        top_snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayer)) {
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR;
        top_snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayerRoot))
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR;
    bool   		 new_variable_layer_editing_active = (new_flags & UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE) != 0;
    bool         new_selected_settings_on_sidebar  = (new_flags & UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR) != 0;
    bool         new_selected_layer_on_sidebar     = (new_flags & UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR) != 0;
    bool         new_selected_layerroot_on_sidebar = (new_flags & UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR) != 0;

    if (this->view3D->get_canvas3d()->get_gizmos_manager().wants_reslice_supports_on_undo())
        top_snapshot_data.flags |= UndoRedo::SnapshotData::RECALCULATE_SLA_SUPPORTS;

    // Disable layer editing before the Undo / Redo jump.
    if (!new_variable_layer_editing_active && view3D->is_layers_editing_enabled())
        view3D->get_canvas3d()->force_main_toolbar_left_action(view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));

    // Make a copy of the snapshot, undo/redo could invalidate the iterator
    const UndoRedo::Snapshot snapshot_copy = *it_snapshot;
    // Do the jump in time.
    if (it_snapshot->timestamp < this->undo_redo_stack().active_snapshot_time() ?
        this->undo_redo_stack().undo(model, this->view3D->get_canvas3d()->get_selection(), this->view3D->get_canvas3d()->get_gizmos_manager(), top_snapshot_data, it_snapshot->timestamp) :
        this->undo_redo_stack().redo(model, this->view3D->get_canvas3d()->get_gizmos_manager(), it_snapshot->timestamp)) {
        if (printer_technology_changed) {
            // Switch to the other printer technology. Switch to the last printer active for that particular technology.
            AppConfig *app_config = wxGetApp().app_config;
            app_config->set("presets", "printer", (new_printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name);
            wxGetApp().preset_bundle->load_presets(*app_config);
			// load_current_presets() calls Tab::load_current_preset() -> TabPrint::update() -> Object_list::update_and_show_object_settings_item(),
			// but the Object list still keeps pointer to the old Model. Avoid a crash by removing selection first.
			this->sidebar->obj_list()->unselect_objects();
            // Load the currently selected preset into the GUI, update the preset selection box.
            // This also switches the printer technology based on the printer technology of the active printer profile.
            wxGetApp().load_current_presets();
        }
        //FIXME updating the Print config from the Wipe tower config values at the ModelWipeTower.
        // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
        if (this->printer_technology == ptFFF) {
            const DynamicPrintConfig &current_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            Vec2d 					  current_position(current_config.opt_float("wipe_tower_x"), current_config.opt_float("wipe_tower_y"));
            double 					  current_rotation = current_config.opt_float("wipe_tower_rotation_angle");
            if (current_position != model.wipe_tower.position || current_rotation != model.wipe_tower.rotation) {
                DynamicPrintConfig new_config;
                new_config.set_key_value("wipe_tower_x", new ConfigOptionFloat(model.wipe_tower.position.x()));
                new_config.set_key_value("wipe_tower_y", new ConfigOptionFloat(model.wipe_tower.position.y()));
                new_config.set_key_value("wipe_tower_rotation_angle", new ConfigOptionFloat(model.wipe_tower.rotation));
                Tab *tab_print = wxGetApp().get_tab(Preset::TYPE_PRINT);
                tab_print->load_config(new_config);
                tab_print->update_dirty();
            }
        }
        // set selection mode for ObjectList on sidebar
        this->sidebar->obj_list()->set_selection_mode(new_selected_settings_on_sidebar  ? ObjectList::SELECTION_MODE::smSettings :
                                                      new_selected_layer_on_sidebar     ? ObjectList::SELECTION_MODE::smLayer :
                                                      new_selected_layerroot_on_sidebar ? ObjectList::SELECTION_MODE::smLayerRoot :
                                                                                          ObjectList::SELECTION_MODE::smUndef);
        if (new_selected_settings_on_sidebar || new_selected_layer_on_sidebar)
            this->sidebar->obj_list()->set_selected_layers_range_idx(layer_range_idx);

        this->update_after_undo_redo(snapshot_copy, temp_snapshot_was_taken);
        // Enable layer editing after the Undo / Redo jump.
        if (! view3D->is_layers_editing_enabled() && this->layers_height_allowed() && new_variable_layer_editing_active)
            view3D->get_canvas3d()->force_main_toolbar_left_action(view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));
    }
}

void Plater::priv::update_after_undo_redo(const UndoRedo::Snapshot& snapshot, bool /* temp_snapshot_was_taken */)
{
    this->view3D->get_canvas3d()->get_selection().clear();
    // Update volumes from the deserializd model, always stop / update the background processing (for both the SLA and FFF technologies).
    this->update((unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE | (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE);
    // Release old snapshots if the memory allocated is excessive. This may remove the top most snapshot if jumping to the very first snapshot.
    //if (temp_snapshot_was_taken)
    // Release the old snapshots always, as it may have happened, that some of the triangle meshes got deserialized from the snapshot, while some
    // triangle meshes may have gotten released from the scene or the background processing, therefore now being calculated into the Undo / Redo stack size.
        this->undo_redo_stack().release_least_recently_used();
    //YS_FIXME update obj_list from the deserialized model (maybe store ObjectIDs into the tree?) (no selections at this point of time)
    this->view3D->get_canvas3d()->get_selection().set_deserialized(GUI::Selection::EMode(this->undo_redo_stack().selection_deserialized().mode), this->undo_redo_stack().selection_deserialized().volumes_and_instances);
    this->view3D->get_canvas3d()->get_gizmos_manager().update_after_undo_redo(snapshot);

    wxGetApp().obj_list()->update_after_undo_redo();

    if (wxGetApp().get_mode() == comSimple && model_has_advanced_features(this->model)) {
        // If the user jumped to a snapshot that require user interface with advanced features, switch to the advanced mode without asking.
        // There is a little risk of surprising the user, as he already must have had the advanced or expert mode active for such a snapshot to be taken.
        Slic3r::GUI::wxGetApp().save_mode(comAdvanced);
        view3D->set_as_dirty();
    }

	// this->update() above was called with POSTPONE_VALIDATION_ERROR_MESSAGE, so that if an error message was generated when updating the back end, it would not open immediately, 
	// but it would be saved to be show later. Let's do it now. We do not want to display the message box earlier, because on Windows & OSX the message box takes over the message
	// queue pump, which in turn executes the rendering function before a full update after the Undo / Redo jump.
	this->show_delayed_error_message();

    //FIXME what about the state of the manipulators?
    //FIXME what about the focus? Cursor in the side panel?

    BOOST_LOG_TRIVIAL(info) << "Undo / Redo snapshot reloaded. Undo / Redo stack memory: " << Slic3r::format_memsize_MB(this->undo_redo_stack().memsize()) << log_memory_info();
}

// Plater / Public

Plater::Plater(wxWindow *parent, MainFrame *main_frame)
    : wxPanel(parent), p(new priv(this, main_frame))
{
    // Initialization performed in the private c-tor
}

Plater::~Plater()
{
}

Sidebar&        Plater::sidebar()           { return *p->sidebar; }
Model&          Plater::model()             { return p->model; }
const Print&    Plater::fff_print() const   { return p->fff_print; }
Print&          Plater::fff_print()         { return p->fff_print; }
const SLAPrint& Plater::sla_print() const   { return p->sla_print; }
SLAPrint&       Plater::sla_print()         { return p->sla_print; }

void Plater::new_project()
{
    p->select_view_3D("3D");
    wxPostEvent(p->view3D->get_wxglcanvas(), SimpleEvent(EVT_GLTOOLBAR_DELETE_ALL));
}

void Plater::load_project()
{
    // Ask user for a project file name.
    wxString input_file;
    wxGetApp().load_project(this, input_file);
    // And finally load the new project.
    load_project(input_file);
}

void Plater::load_project(const wxString& filename)
{
    if (filename.empty())
        return;

    // Take the Undo / Redo snapshot.
    Plater::TakeSnapshot snapshot(this, _(L("Load Project")) + ": " + wxString::FromUTF8(into_path(filename).stem().string().c_str()));

    p->reset();

    std::vector<fs::path> input_paths;
    input_paths.push_back(into_path(filename));

    std::vector<size_t> res = load_files(input_paths);

    // if res is empty no data has been loaded
    if (!res.empty())
        p->set_project_filename(filename);
}

void Plater::add_model()
{
    wxArrayString input_files;
    wxGetApp().import_model(this, input_files);
    if (input_files.empty())
        return;

    std::vector<fs::path> paths;
    for (const auto &file : input_files)
        paths.push_back(into_path(file));

    wxString snapshot_label;
    assert(! paths.empty());
    if (paths.size() == 1) {
        snapshot_label = _(L("Import Object"));
        snapshot_label += ": ";
        snapshot_label += wxString::FromUTF8(paths.front().filename().string().c_str());
    } else {
        snapshot_label = _(L("Import Objects"));
        snapshot_label += ": ";
        snapshot_label += wxString::FromUTF8(paths.front().filename().string().c_str());
        for (size_t i = 1; i < paths.size(); ++ i) {
            snapshot_label += ", ";
            snapshot_label += wxString::FromUTF8(paths[i].filename().string().c_str());
        }
    }

    Plater::TakeSnapshot snapshot(this, snapshot_label);
    load_files(paths, true, false);
}

void Plater::extract_config_from_project()
{
    wxString input_file;
    wxGetApp().load_project(this, input_file);

    if (input_file.empty())
        return;

    std::vector<fs::path> input_paths;
    input_paths.push_back(into_path(input_file));
    load_files(input_paths, false, true);
}

std::vector<size_t> Plater::load_files(const std::vector<fs::path>& input_files, bool load_model, bool load_config) { return p->load_files(input_files, load_model, load_config); }

// To be called when providing a list of files to the GUI slic3r on command line.
std::vector<size_t> Plater::load_files(const std::vector<std::string>& input_files, bool load_model, bool load_config)
{
    std::vector<fs::path> paths;
    paths.reserve(input_files.size());
    for (const std::string& path : input_files)
        paths.emplace_back(path);
    return p->load_files(paths, load_model, load_config);
}

void Plater::update() { p->update(); }

void Plater::stop_jobs() { p->m_ui_jobs.stop_all(); }

void Plater::update_ui_from_settings() { p->update_ui_from_settings(); }

void Plater::select_view(const std::string& direction) { p->select_view(direction); }

void Plater::select_view_3D(const std::string& name) { p->select_view_3D(name); }

bool Plater::is_preview_shown() const { return p->is_preview_shown(); }
bool Plater::is_preview_loaded() const { return p->is_preview_loaded(); }
bool Plater::is_view3D_shown() const { return p->is_view3D_shown(); }

bool Plater::are_view3D_labels_shown() const { return p->are_view3D_labels_shown(); }
void Plater::show_view3D_labels(bool show) { p->show_view3D_labels(show); }

void Plater::select_all() { p->select_all(); }
void Plater::deselect_all() { p->deselect_all(); }

void Plater::remove(size_t obj_idx) { p->remove(obj_idx); }
void Plater::reset() { p->reset(); }
void Plater::reset_with_confirm()
{
    if (wxMessageDialog((wxWindow*)this, _(L("All objects will be removed, continue?")), wxString(SLIC3R_APP_NAME) + " - " + _(L("Delete all")), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE).ShowModal() == wxID_YES)
        reset();
}

void Plater::delete_object_from_model(size_t obj_idx) { p->delete_object_from_model(obj_idx); }

void Plater::remove_selected()
{
    Plater::TakeSnapshot snapshot(this, _(L("Delete Selected Objects")));
    this->p->view3D->delete_selected();
}

void Plater::increase_instances(size_t num)
{
    if (! can_increase_instances()) { return; }

    Plater::TakeSnapshot snapshot(this, _(L("Increase Instances")));

    int obj_idx = p->get_selected_object_idx();

    ModelObject* model_object = p->model.objects[obj_idx];
    ModelInstance* model_instance = model_object->instances.back();

    bool was_one_instance = model_object->instances.size()==1;

    double offset_base = canvas3D()->get_size_proportional_to_max_bed_size(0.05);
    double offset = offset_base;
    for (size_t i = 0; i < num; i++, offset += offset_base) {
        Vec3d offset_vec = model_instance->get_offset() + Vec3d(offset, offset, 0.0);
        model_object->add_instance(offset_vec, model_instance->get_scaling_factor(), model_instance->get_rotation(), model_instance->get_mirror());
//        p->print.get_object(obj_idx)->add_copy(Slic3r::to_2d(offset_vec));
    }

    sidebar().obj_list()->increase_object_instances(obj_idx, was_one_instance ? num + 1 : num);

    if (p->get_config("autocenter") == "1")
        p->arrange();

    p->update();

    p->get_selection().add_instance(obj_idx, (int)model_object->instances.size() - 1);

    p->selection_changed();

    this->p->schedule_background_process();
}

void Plater::decrease_instances(size_t num)
{
    if (! can_decrease_instances()) { return; }

    Plater::TakeSnapshot snapshot(this, _(L("Decrease Instances")));

    int obj_idx = p->get_selected_object_idx();

    ModelObject* model_object = p->model.objects[obj_idx];
    if (model_object->instances.size() > num) {
        for (size_t i = 0; i < num; ++ i)
            model_object->delete_last_instance();
        p->update();
        // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
        sidebar().obj_list()->decrease_object_instances(obj_idx, num);
    }
    else {
        remove(obj_idx);
    }

    if (!model_object->instances.empty())
        p->get_selection().add_instance(obj_idx, (int)model_object->instances.size() - 1);

    p->selection_changed();
    this->p->schedule_background_process();
}

void Plater::set_number_of_copies(/*size_t num*/)
{
    int obj_idx = p->get_selected_object_idx();
    if (obj_idx == -1)
        return;

    ModelObject* model_object = p->model.objects[obj_idx];

    const int num = wxGetNumberFromUser( " ", _(L("Enter the number of copies:")),
                                    _(L("Copies of the selected object")), model_object->instances.size(), 0, 1000, this );
    if (num < 0)
        return;

    Plater::TakeSnapshot snapshot(this, wxString::Format(_(L("Set numbers of copies to %d")), num));

    int diff = num - (int)model_object->instances.size();
    if (diff > 0)
        increase_instances(diff);
    else if (diff < 0)
        decrease_instances(-diff);
}

bool Plater::is_selection_empty() const
{
    return p->get_selection().is_empty() || p->get_selection().is_wipe_tower();
}

void Plater::scale_selection_to_fit_print_volume()
{
    p->scale_selection_to_fit_print_volume();
}

void Plater::cut(size_t obj_idx, size_t instance_idx, coordf_t z, bool keep_upper, bool keep_lower, bool rotate_lower)
{
    wxCHECK_RET(obj_idx < p->model.objects.size(), "obj_idx out of bounds");
    auto *object = p->model.objects[obj_idx];

    wxCHECK_RET(instance_idx < object->instances.size(), "instance_idx out of bounds");

    if (!keep_upper && !keep_lower) {
        return;
    }

    Plater::TakeSnapshot snapshot(this, _(L("Cut by Plane")));

    wxBusyCursor wait;
    const auto new_objects = object->cut(instance_idx, z, keep_upper, keep_lower, rotate_lower);

    remove(obj_idx);
    p->load_model_objects(new_objects);

    Selection& selection = p->get_selection();
    size_t last_id = p->model.objects.size() - 1;
    for (size_t i = 0; i < new_objects.size(); ++i)
    {
        selection.add_object((unsigned int)(last_id - i), i == 0);
    }
}

void Plater::export_gcode(bool prefer_removable)
{
    if (p->model.objects.empty())
        return;

    // If possible, remove accents from accented latin characters.
    // This function is useful for generating file names to be processed by legacy firmwares.
    fs::path default_output_file;
    try {
        // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
        // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
        unsigned int state = this->p->update_restart_background_process(false, false);
        if (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID)
            return;
        default_output_file = this->p->background_process.output_filepath_for_project(into_path(get_project_filename(".3mf")));
    }
    catch (const std::exception &ex) {
        show_error(this, ex.what());
        return;
    }
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));
    AppConfig 				&appconfig 				 = *wxGetApp().app_config;
    RemovableDriveManager 	&removable_drive_manager = *wxGetApp().removable_drive_manager();
    // Get a last save path, either to removable media or to an internal media.
    std::string      		 start_dir 				 = appconfig.get_last_output_dir(default_output_file.parent_path().string(), prefer_removable);
	if (prefer_removable) {
		// Returns a path to a removable media if it exists, prefering start_dir. Update the internal removable drives database.
		start_dir = removable_drive_manager.get_removable_drive_path(start_dir);
		if (start_dir.empty())
			// Direct user to the last internal media.
			start_dir = appconfig.get_last_output_dir(default_output_file.parent_path().string(), false);
	}

    fs::path output_path;
    {
        wxFileDialog dlg(this, (printer_technology() == ptFFF) ? _(L("Save G-code file as:")) : _(L("Save SL1 file as:")),
            start_dir,
            from_path(default_output_file.filename()),
            GUI::file_wildcards((printer_technology() == ptFFF) ? FT_GCODE : FT_PNGZIP, default_output_file.extension().string()),
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT
        );
        if (dlg.ShowModal() == wxID_OK)
            output_path = into_path(dlg.GetPath());
    }

    if (! output_path.empty()) {
		bool path_on_removable_media = removable_drive_manager.set_and_verify_last_save_path(output_path.string());
        p->export_gcode(output_path, path_on_removable_media, PrintHostJob());
        // Storing a path to AppConfig either as path to removable media or a path to internal media.
        // is_path_on_removable_drive() is called with the "true" parameter to update its internal database as the user may have shuffled the external drives
        // while the dialog was open.
        appconfig.update_last_output_dir(output_path.parent_path().string(), path_on_removable_media);
		p->writing_to_removable_device = path_on_removable_media;
	}
}

void Plater::export_stl(bool extended, bool selection_only)
{
    if (p->model.objects.empty()) { return; }

    wxString path = p->get_export_file(FT_STL);
    if (path.empty()) { return; }
    const std::string path_u8 = into_u8(path);

    wxBusyCursor wait;

    const auto &selection = p->get_selection();
    const auto obj_idx = selection.get_object_idx();
    if (selection_only && (obj_idx == -1 || selection.is_wipe_tower()))
        return;

    TriangleMesh mesh;
    if (p->printer_technology == ptFFF) {
        if (selection_only) {
            const ModelObject* model_object = p->model.objects[obj_idx];
            if (selection.get_mode() == Selection::Instance)
            {
                if (selection.is_single_full_object())
                    mesh = model_object->mesh();
                else
                    mesh = model_object->full_raw_mesh();
            }
            else
            {
                const GLVolume* volume = selection.get_volume(*selection.get_volume_idxs().begin());
                mesh = model_object->volumes[volume->volume_idx()]->mesh();
                mesh.transform(volume->get_volume_transformation().get_matrix());
                mesh.translate(-model_object->origin_translation.cast<float>());
            }
        }
        else {
            mesh = p->model.mesh();
        }
    }
    else
    {
        // This is SLA mode, all objects have only one volume.
        // However, we must have a look at the backend to load
        // hollowed mesh and/or supports

        const PrintObjects& objects = p->sla_print.objects();
        for (const SLAPrintObject* object : objects)
        {
            const ModelObject* model_object = object->model_object();
            if (selection_only) {
                if (model_object->id() != p->model.objects[obj_idx]->id())
                    continue;
            }
            Transform3d mesh_trafo_inv = object->trafo().inverse();
            bool is_left_handed = object->is_left_handed();

            TriangleMesh pad_mesh;
            bool has_pad_mesh = extended && object->has_mesh(slaposPad);
            if (has_pad_mesh)
            {
                pad_mesh = object->get_mesh(slaposPad);
                pad_mesh.transform(mesh_trafo_inv);
            }

            TriangleMesh supports_mesh;
            bool has_supports_mesh = extended && object->has_mesh(slaposSupportTree);
            if (has_supports_mesh)
            {
                supports_mesh = object->get_mesh(slaposSupportTree);
                supports_mesh.transform(mesh_trafo_inv);
            }
            const std::vector<SLAPrintObject::Instance>& obj_instances = object->instances();
            for (const SLAPrintObject::Instance& obj_instance : obj_instances)
            {
                auto it = std::find_if(model_object->instances.begin(), model_object->instances.end(),
                    [&obj_instance](const ModelInstance *mi) { return mi->id() == obj_instance.instance_id; });
                assert(it != model_object->instances.end());

                if (it != model_object->instances.end())
                {
                    bool one_inst_only = selection_only && ! selection.is_single_full_object();

                    int instance_idx = it - model_object->instances.begin();
                    const Transform3d& inst_transform = one_inst_only
                            ? Transform3d::Identity()
                            : object->model_object()->instances[instance_idx]->get_transformation().get_matrix();

                    if (has_pad_mesh)
                    {
                        TriangleMesh inst_pad_mesh = pad_mesh;
                        inst_pad_mesh.transform(inst_transform, is_left_handed);
                        mesh.merge(inst_pad_mesh);
                    }

                    if (has_supports_mesh)
                    {
                        TriangleMesh inst_supports_mesh = supports_mesh;
                        inst_supports_mesh.transform(inst_transform, is_left_handed);
                        mesh.merge(inst_supports_mesh);
                    }

                    TriangleMesh inst_object_mesh = object->get_mesh_to_print();
                    inst_object_mesh.transform(mesh_trafo_inv);
                    inst_object_mesh.transform(inst_transform, is_left_handed);

                    mesh.merge(inst_object_mesh);

                    if (one_inst_only)
                        break;
                }
            }
        }
    }

    Slic3r::store_stl(path_u8.c_str(), &mesh, true);
    p->statusbar()->set_status_text(from_u8((boost::format(_utf8(L("STL file exported to %s"))) % path).str()));
}

void Plater::export_amf()
{
    if (p->model.objects.empty()) { return; }

    wxString path = p->get_export_file(FT_AMF);
    if (path.empty()) { return; }
    const std::string path_u8 = into_u8(path);

    wxBusyCursor wait;
    bool export_config = true;
    DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config_secure();
    bool full_pathnames = wxGetApp().app_config->get("export_sources_full_pathnames") == "1";
    if (Slic3r::store_amf(path_u8.c_str(), &p->model, export_config ? &cfg : nullptr, full_pathnames)) {
        // Success
        p->statusbar()->set_status_text(from_u8((boost::format(_utf8(L("AMF file exported to %s"))) % path).str()));
    } else {
        // Failure
        p->statusbar()->set_status_text(from_u8((boost::format(_utf8(L("Error exporting AMF file %s"))) % path).str()));
    }
}

void Plater::export_3mf(const boost::filesystem::path& output_path)
{
    if (p->model.objects.empty()) { return; }

    wxString path;
    bool export_config = true;
    if (output_path.empty())
    {
        path = p->get_export_file(FT_3MF);
        if (path.empty()) { return; }
    }
    else
        path = from_path(output_path);

    if (!path.Lower().EndsWith(".3mf"))
        return;

    DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config_secure();
    const std::string path_u8 = into_u8(path);
    wxBusyCursor wait;
    bool full_pathnames = wxGetApp().app_config->get("export_sources_full_pathnames") == "1";
    ThumbnailData thumbnail_data;
    p->generate_thumbnail(thumbnail_data, THUMBNAIL_SIZE_3MF.first, THUMBNAIL_SIZE_3MF.second, false, true, true, true);
    if (Slic3r::store_3mf(path_u8.c_str(), &p->model, export_config ? &cfg : nullptr, full_pathnames, &thumbnail_data)) {
        // Success
        p->statusbar()->set_status_text(from_u8((boost::format(_utf8(L("3MF file exported to %s"))) % path).str()));
        p->set_project_filename(path);
    }
    else {
        // Failure
        p->statusbar()->set_status_text(from_u8((boost::format(_utf8(L("Error exporting 3MF file %s"))) % path).str()));
    }
}

void Plater::reload_from_disk()
{
    p->reload_from_disk();
}

void Plater::reload_all_from_disk()
{
    p->reload_all_from_disk();
}

bool Plater::has_toolpaths_to_export() const
{
    return  p->preview->get_canvas3d()->has_toolpaths_to_export();
}

void Plater::export_toolpaths_to_obj() const
{
    if ((printer_technology() != ptFFF) || !is_preview_loaded())
        return;

    wxString path = p->get_export_file(FT_OBJ);
    if (path.empty()) 
        return;
    
    wxBusyCursor wait;
    p->preview->get_canvas3d()->export_toolpaths_to_obj(into_u8(path).c_str());
}


void Plater::reslice()
{
    // Stop arrange and (or) optimize rotation tasks.
    this->stop_jobs();

    if (printer_technology() == ptSLA) {
        for (auto& object : model().objects)
            if (object->sla_points_status == sla::PointsStatus::NoPoints)
                object->sla_points_status = sla::PointsStatus::Generating;
    }

    //FIXME Don't reslice if export of G-code or sending to OctoPrint is running.
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->p->update_background_process(true);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        this->p->view3D->reload_scene(false);
    // If the SLA processing of just a single object's supports is running, restart slicing for the whole object.
    this->p->background_process.set_task(PrintBase::TaskParams());
    // Only restarts if the state is valid.
    this->p->restart_background_process(state | priv::UPDATE_BACKGROUND_PROCESS_FORCE_RESTART);

    if ((state & priv::UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
        return;

    if (p->background_process.running())
    {
        if (wxGetApp().get_mode() == comSimple)
            p->sidebar->set_btn_label(ActionButtonType::abReslice, _(L("Slicing")) + dots);
        else
        {
            p->sidebar->set_btn_label(ActionButtonType::abReslice, _(L("Slice now")));
            p->show_action_buttons(false);
        }
    }
    else if (!p->background_process.empty() && !p->background_process.idle())
        p->show_action_buttons(true);

    // update type of preview
    p->preview->update_view_type(true);
}

void Plater::reslice_SLA_supports(const ModelObject &object, bool postpone_error_messages)
{
    reslice_SLA_until_step(slaposPad, object, postpone_error_messages);
}

void Plater::reslice_SLA_hollowing(const ModelObject &object, bool postpone_error_messages)
{
    reslice_SLA_until_step(slaposDrillHoles, object, postpone_error_messages);
}

void Plater::reslice_SLA_until_step(SLAPrintObjectStep step, const ModelObject &object, bool postpone_error_messages)
{
    //FIXME Don't reslice if export of G-code or sending to OctoPrint is running.
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->p->update_background_process(true, postpone_error_messages);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        this->p->view3D->reload_scene(false);

    if (this->p->background_process.empty() || (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID))
        // Nothing to do on empty input or invalid configuration.
        return;

    // Limit calculation to the single object only.
    PrintBase::TaskParams task;
    task.single_model_object = object.id();
    // If the background processing is not enabled, calculate supports just for the single instance.
    // Otherwise calculate everything, but start with the provided object.
    if (!this->p->background_processing_enabled()) {
        task.single_model_instance_only = true;
        task.to_object_step = step;
    }
    this->p->background_process.set_task(task);
    // and let the background processing start.
    this->p->restart_background_process(state | priv::UPDATE_BACKGROUND_PROCESS_FORCE_RESTART);
}

void Plater::send_gcode()
{
    if (p->model.objects.empty()) { return; }

    PrintHostJob upload_job(p->config);
    if (upload_job.empty()) { return; }

    // Obtain default output path
    fs::path default_output_file;
    try {
        // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
        // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
        unsigned int state = this->p->update_restart_background_process(false, false);
        if (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID)
            return;
        default_output_file = this->p->background_process.output_filepath_for_project(into_path(get_project_filename(".3mf")));
    }
    catch (const std::exception &ex) {
        show_error(this, ex.what());
        return;
    }
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));

    PrintHostSendDialog dlg(default_output_file, upload_job.printhost->can_start_print());
    if (dlg.ShowModal() == wxID_OK) {
        upload_job.upload_data.upload_path = dlg.filename();
        upload_job.upload_data.start_print = dlg.start_print();

        p->export_gcode(fs::path(), false, std::move(upload_job));
    }
}

// Called when the Eject button is pressed.
void Plater::eject_drive()
{
    wxBusyCursor wait;
	wxGetApp().removable_drive_manager()->eject_drive();
}

void Plater::take_snapshot(const std::string &snapshot_name) { p->take_snapshot(snapshot_name); }
void Plater::take_snapshot(const wxString &snapshot_name) { p->take_snapshot(snapshot_name); }
void Plater::suppress_snapshots() { p->suppress_snapshots(); }
void Plater::allow_snapshots() { p->allow_snapshots(); }
void Plater::undo() { p->undo(); }
void Plater::redo() { p->redo(); }
void Plater::undo_to(int selection)
{
    if (selection == 0) {
        p->undo();
        return;
    }

    const int idx = p->get_active_snapshot_index() - selection - 1;
    p->undo_redo_to(p->undo_redo_stack().snapshots()[idx].timestamp);
}
void Plater::redo_to(int selection)
{
    if (selection == 0) {
        p->redo();
        return;
    }

    const int idx = p->get_active_snapshot_index() + selection + 1;
    p->undo_redo_to(p->undo_redo_stack().snapshots()[idx].timestamp);
}
bool Plater::undo_redo_string_getter(const bool is_undo, int idx, const char** out_text)
{
    const std::vector<UndoRedo::Snapshot>& ss_stack = p->undo_redo_stack().snapshots();
    const int idx_in_ss_stack = p->get_active_snapshot_index() + (is_undo ? -(++idx) : idx);

    if (0 < idx_in_ss_stack && (size_t)idx_in_ss_stack < ss_stack.size() - 1) {
        *out_text = ss_stack[idx_in_ss_stack].name.c_str();
        return true;
    }

    return false;
}

void Plater::undo_redo_topmost_string_getter(const bool is_undo, std::string& out_text)
{
    const std::vector<UndoRedo::Snapshot>& ss_stack = p->undo_redo_stack().snapshots();
    const int idx_in_ss_stack = p->get_active_snapshot_index() + (is_undo ? -1 : 0);

    if (0 < idx_in_ss_stack && (size_t)idx_in_ss_stack < ss_stack.size() - 1) {
        out_text = ss_stack[idx_in_ss_stack].name;
        return;
    }

    out_text = "";
}

void Plater::on_extruders_change(size_t num_extruders)
{
    auto& choices = sidebar().combos_filament();

    if (num_extruders == choices.size())
        return;

    wxWindowUpdateLocker noUpdates_scrolled_panel(&sidebar()/*.scrolled_panel()*/);

    size_t i = choices.size();
    while ( i < num_extruders )
    {
        PresetComboBox* choice/*{ nullptr }*/;
        sidebar().init_filament_combo(&choice, i);
        choices.push_back(choice);

        // initialize selection
        wxGetApp().preset_bundle->update_plater_filament_ui(i, choice);
        ++i;
    }

    // remove unused choices if any
    sidebar().remove_unused_filament_combos(num_extruders);

    sidebar().Layout();
    sidebar().scrolled_panel()->Refresh();
}

void Plater::on_config_change(const DynamicPrintConfig &config)
{
    bool update_scheduled = false;
    bool bed_shape_changed = false;
    for (auto opt_key : p->config->diff(config)) {
        if (opt_key == "filament_colour")
        {
            update_scheduled = true; // update should be scheduled (for update 3DScene) #2738

            /* There is a case, when we use filament_color instead of extruder_color (when extruder_color == "").
             * Thus plater config option "filament_colour" should be filled with filament_presets values.
             * Otherwise, on 3dScene will be used last edited filament color for all volumes with extruder_color == "".
             */
            const std::vector<std::string> filament_presets = wxGetApp().preset_bundle->filament_presets;
            if (filament_presets.size() > 1 &&
                p->config->option<ConfigOptionStrings>(opt_key)->values.size() != config.option<ConfigOptionStrings>(opt_key)->values.size())
            {
                const PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
                std::vector<std::string> filament_colors;
                filament_colors.reserve(filament_presets.size());

                for (const std::string& filament_preset : filament_presets)
                    filament_colors.push_back(filaments.find_preset(filament_preset, true)->config.opt_string("filament_colour", (unsigned)0));

                p->config->option<ConfigOptionStrings>(opt_key)->values = filament_colors;
                p->sidebar->obj_list()->update_extruder_colors();
                continue;
            }
        }
        
        p->config->set_key_value(opt_key, config.option(opt_key)->clone());
        if (opt_key == "printer_technology")
            this->set_printer_technology(config.opt_enum<PrinterTechnology>(opt_key));
        else if ((opt_key == "bed_shape") || (opt_key == "bed_custom_texture") || (opt_key == "bed_custom_model")) {
            bed_shape_changed = true;
            update_scheduled = true;
        }
        else if (boost::starts_with(opt_key, "wipe_tower") ||
            // opt_key == "filament_minimal_purge_on_wipe_tower" // ? #ys_FIXME
            opt_key == "single_extruder_multi_material") {
            update_scheduled = true;
        }
        else if(opt_key == "variable_layer_height") {
            if (p->config->opt_bool("variable_layer_height") != true) {
                p->view3D->enable_layers_editing(false);
                p->view3D->set_as_dirty();
            }
        }
        else if(opt_key == "extruder_colour") {
            update_scheduled = true;
            p->preview->set_number_extruders(p->config->option<ConfigOptionStrings>(opt_key)->values.size());
            p->sidebar->obj_list()->update_extruder_colors();
        } else if(opt_key == "max_print_height") {
            update_scheduled = true;
        }
        else if (opt_key == "printer_model") {
            // update to force bed selection(for texturing)
            bed_shape_changed = true;
            update_scheduled = true;
        }
    }

    if (bed_shape_changed)
        p->set_bed_shape(p->config->option<ConfigOptionPoints>("bed_shape")->values,
            p->config->option<ConfigOptionString>("bed_custom_texture")->value,
            p->config->option<ConfigOptionString>("bed_custom_model")->value);

    if (update_scheduled)
        update();

    if (p->main_frame->is_loaded())
        this->p->schedule_background_process();
}

void Plater::set_bed_shape() const
{
	p->set_bed_shape(p->config->option<ConfigOptionPoints>("bed_shape")->values,
		p->config->option<ConfigOptionString>("bed_custom_texture")->value,
		p->config->option<ConfigOptionString>("bed_custom_model")->value);
}

void Plater::force_filament_colors_update()
{
    bool update_scheduled = false;
    DynamicPrintConfig* config = p->config;
    const std::vector<std::string> filament_presets = wxGetApp().preset_bundle->filament_presets;
    if (filament_presets.size() > 1 && 
        p->config->option<ConfigOptionStrings>("filament_colour")->values.size() == filament_presets.size())
    {
        const PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
        std::vector<std::string> filament_colors;
        filament_colors.reserve(filament_presets.size());

        for (const std::string& filament_preset : filament_presets)
            filament_colors.push_back(filaments.find_preset(filament_preset, true)->config.opt_string("filament_colour", (unsigned)0));

        if (config->option<ConfigOptionStrings>("filament_colour")->values != filament_colors) {
            config->option<ConfigOptionStrings>("filament_colour")->values = filament_colors;
            update_scheduled = true;
        }
    }

    if (update_scheduled) {
        update();
        p->sidebar->obj_list()->update_extruder_colors();
    }

    if (p->main_frame->is_loaded())
        this->p->schedule_background_process();
}

void Plater::force_print_bed_update()
{
	// Fill in the printer model key with something which cannot possibly be valid, so that Plater::on_config_change() will update the print bed
	// once a new Printer profile config is loaded.
	p->config->opt_string("printer_model", true) = "\x01\x00\x01";
}

void Plater::on_activate()
{
#ifdef __linux__
    wxWindow *focus_window = wxWindow::FindFocus();
    // Activating the main frame, and no window has keyboard focus.
    // Set the keyboard focus to the visible Canvas3D.
    if (this->p->view3D->IsShown() && (!focus_window || focus_window == this->p->view3D->get_wxglcanvas()))
        this->p->view3D->get_wxglcanvas()->SetFocus();

    else if (this->p->preview->IsShown() && (!focus_window || focus_window == this->p->view3D->get_wxglcanvas()))
        this->p->preview->get_wxglcanvas()->SetFocus();
#endif

	this->p->show_delayed_error_message();
}

// Get vector of extruder colors considering filament color, if extruder color is undefined.
std::vector<std::string> Plater::get_extruder_colors_from_plater_config() const
{
    const Slic3r::DynamicPrintConfig* config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    std::vector<std::string> extruder_colors;
    if (!config->has("extruder_colour")) // in case of a SLA print
        return extruder_colors;

    extruder_colors = (config->option<ConfigOptionStrings>("extruder_colour"))->values;
    if (!wxGetApp().plater())
        return extruder_colors;

    const std::vector<std::string>& filament_colours = (p->config->option<ConfigOptionStrings>("filament_colour"))->values;
    for (size_t i = 0; i < extruder_colors.size(); ++i)
        if (extruder_colors[i] == "" && i < filament_colours.size())
            extruder_colors[i] = filament_colours[i];

    return extruder_colors;
}

/* Get vector of colors used for rendering of a Preview scene in "Color print" mode
 * It consists of extruder colors and colors, saved in model.custom_gcode_per_print_z
 */
std::vector<std::string> Plater::get_colors_for_color_print() const
{
    std::vector<std::string> colors = get_extruder_colors_from_plater_config();
    colors.reserve(colors.size() + p->model.custom_gcode_per_print_z.gcodes.size());

    for (const CustomGCode::Item& code : p->model.custom_gcode_per_print_z.gcodes)
        if (code.gcode == ColorChangeCode)
            colors.emplace_back(code.color);

    return colors;
}

wxString Plater::get_project_filename(const wxString& extension) const
{
    return p->get_project_filename(extension);
}

void Plater::set_project_filename(const wxString& filename)
{
    return p->set_project_filename(filename);
}

bool Plater::is_export_gcode_scheduled() const
{
    return p->background_process.is_export_scheduled();
}

int Plater::get_selected_object_idx()
{
    return p->get_selected_object_idx();
}

bool Plater::is_single_full_object_selection() const
{
    return p->get_selection().is_single_full_object();
}

GLCanvas3D* Plater::canvas3D()
{
    return p->view3D->get_canvas3d();
}

GLCanvas3D* Plater::get_current_canvas3D()
{
    return p->get_current_canvas3D();
}

BoundingBoxf Plater::bed_shape_bb() const
{
    return p->bed_shape_bb();
}

void Plater::set_current_canvas_as_dirty()
{
    p->set_current_canvas_as_dirty();
}

PrinterTechnology Plater::printer_technology() const
{
    return p->printer_technology;
}

void Plater::set_printer_technology(PrinterTechnology printer_technology)
{
    p->printer_technology = printer_technology;
    if (p->background_process.select_technology(printer_technology)) {
        // Update the active presets.
    }
    //FIXME for SLA synchronize
    //p->background_process.apply(Model)!

    p->label_btn_export = printer_technology == ptFFF ? L("Export G-code") : L("Export");
    p->label_btn_send   = printer_technology == ptFFF ? L("Send G-code")   : L("Send to printer");

    if (wxGetApp().mainframe)
        wxGetApp().mainframe->update_menubar();
}

void Plater::changed_object(int obj_idx)
{
    if (obj_idx < 0)
        return;
    // recenter and re - align to Z = 0
    auto model_object = p->model.objects[obj_idx];
    model_object->ensure_on_bed();
    if (this->p->printer_technology == ptSLA) {
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data, update the 3D scene.
        this->p->update_restart_background_process(true, false);
    }
    else
        p->view3D->reload_scene(false);

    // update print
    this->p->schedule_background_process();
}

void Plater::changed_objects(const std::vector<size_t>& object_idxs)
{
    if (object_idxs.empty())
        return;

    for (size_t obj_idx : object_idxs)
    {
        if (obj_idx < p->model.objects.size())
            // recenter and re - align to Z = 0
            p->model.objects[obj_idx]->ensure_on_bed();
    }
    if (this->p->printer_technology == ptSLA) {
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data, update the 3D scene.
        this->p->update_restart_background_process(true, false);
    }
    else
        p->view3D->reload_scene(false);

    // update print
    this->p->schedule_background_process();
}

void Plater::schedule_background_process(bool schedule/* = true*/)
{
    if (schedule)
        this->p->schedule_background_process();

    this->p->suppressed_backround_processing_update = false;
}

bool Plater::is_background_process_update_scheduled() const
{
    return this->p->background_process_timer.IsRunning();
}

void Plater::suppress_background_process(const bool stop_background_process)
{
    if (stop_background_process)
        this->p->background_process_timer.Stop();

    this->p->suppressed_backround_processing_update = true;
}

void Plater::fix_through_netfabb(const int obj_idx, const int vol_idx/* = -1*/) { p->fix_through_netfabb(obj_idx, vol_idx); }

void Plater::update_object_menu() { p->update_object_menu(); }
void Plater::show_action_buttons(const bool ready_to_slice) const { p->show_action_buttons(ready_to_slice); }

void Plater::copy_selection_to_clipboard()
{
    if (can_copy_to_clipboard())
        p->view3D->get_canvas3d()->get_selection().copy_to_clipboard();
}

void Plater::paste_from_clipboard()
{
    if (!can_paste_from_clipboard())
        return;

    Plater::TakeSnapshot snapshot(this, _(L("Paste From Clipboard")));
    p->view3D->get_canvas3d()->get_selection().paste_from_clipboard();
}

void Plater::msw_rescale()
{
    p->preview->msw_rescale();

    p->view3D->get_canvas3d()->msw_rescale();

    p->sidebar->msw_rescale();

    p->msw_rescale_object_menu();

    Layout();
    GetParent()->Layout();
}

bool Plater::init_view_toolbar()
{
    return p->init_view_toolbar();
}

const Camera& Plater::get_camera() const
{
    return p->camera;
}

Camera& Plater::get_camera()
{
    return p->camera;
}

const Mouse3DController& Plater::get_mouse3d_controller() const
{
    return p->mouse3d_controller;
}

Mouse3DController& Plater::get_mouse3d_controller()
{
    return p->mouse3d_controller;
}

bool Plater::can_delete() const { return p->can_delete(); }
bool Plater::can_delete_all() const { return p->can_delete_all(); }
bool Plater::can_increase_instances() const { return p->can_increase_instances(); }
bool Plater::can_decrease_instances() const { return p->can_decrease_instances(); }
bool Plater::can_set_instance_to_object() const { return p->can_set_instance_to_object(); }
bool Plater::can_fix_through_netfabb() const { return p->can_fix_through_netfabb(); }
bool Plater::can_split_to_objects() const { return p->can_split_to_objects(); }
bool Plater::can_split_to_volumes() const { return p->can_split_to_volumes(); }
bool Plater::can_arrange() const { return p->can_arrange(); }
bool Plater::can_layers_editing() const { return p->can_layers_editing(); }
bool Plater::can_paste_from_clipboard() const
{
    const Selection& selection = p->view3D->get_canvas3d()->get_selection();
    const Selection::Clipboard& clipboard = selection.get_clipboard();

    if (clipboard.is_empty())
        return false;

    if ((wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA) && !clipboard.is_sla_compliant())
        return false;

    Selection::EMode mode = clipboard.get_mode();
    if ((mode == Selection::Volume) && !selection.is_from_single_instance())
        return false;

    if ((mode == Selection::Instance) && (selection.get_mode() != Selection::Instance))
        return false;

    return true;
}

bool Plater::can_copy_to_clipboard() const
{
    if (is_selection_empty())
        return false;

    const Selection& selection = p->view3D->get_canvas3d()->get_selection();
    if ((wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA) && !selection.is_sla_compliant())
        return false;

    return true;
}

bool Plater::can_undo() const { return p->undo_redo_stack().has_undo_snapshot(); }
bool Plater::can_redo() const { return p->undo_redo_stack().has_redo_snapshot(); }
bool Plater::can_reload_from_disk() const { return p->can_reload_from_disk(); }
const UndoRedo::Stack& Plater::undo_redo_stack_main() const { return p->undo_redo_stack_main(); }
void Plater::enter_gizmos_stack() { p->enter_gizmos_stack(); }
void Plater::leave_gizmos_stack() { p->leave_gizmos_stack(); }
bool Plater::inside_snapshot_capture() { return p->inside_snapshot_capture(); }

// Wrapper around wxWindow::PopupMenu to suppress error messages popping out while tracking the popup menu.
bool Plater::PopupMenu(wxMenu *menu, const wxPoint& pos)
{
	// Don't want to wake up and trigger reslicing while tracking the pop-up menu.
	SuppressBackgroundProcessingUpdate sbpu;
	// When tracking a pop-up menu, postpone error messages from the slicing result.
	m_tracking_popup_menu = true;
	bool out = this->wxPanel::PopupMenu(menu, pos);
	m_tracking_popup_menu = false;
	if (! m_tracking_popup_menu_error_message.empty()) {
        // Don't know whether the CallAfter is necessary, but it should not hurt.
        // The menus likely sends out some commands, so we may be safer if the dialog is shown after the menu command is processed.
		wxString message = std::move(m_tracking_popup_menu_error_message);
        wxTheApp->CallAfter([message, this]() { show_error(this, message); });
        m_tracking_popup_menu_error_message.clear();
    }
	return out;
}

SuppressBackgroundProcessingUpdate::SuppressBackgroundProcessingUpdate() :
    m_was_scheduled(wxGetApp().plater()->is_background_process_update_scheduled())
{
    wxGetApp().plater()->suppress_background_process(m_was_scheduled);
}

SuppressBackgroundProcessingUpdate::~SuppressBackgroundProcessingUpdate()
{
    wxGetApp().plater()->schedule_background_process(m_was_scheduled);
}

}}    // namespace Slic3r::GUI
