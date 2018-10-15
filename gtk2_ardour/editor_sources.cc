/*
    Copyright (C) 2000-2005 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>
#include <sstream>

#include "pbd/basename.h"
#include "pbd/enumwriter.h"

#include "ardour/audioregion.h"
#include "ardour/source.h"
#include "ardour/audiofilesource.h"
#include "ardour/silentfilesource.h"
#include "ardour/region_factory.h"
#include "ardour/session.h"
#include "ardour/profile.h"

#include "gtkmm2ext/treeutils.h"
#include "gtkmm2ext/utils.h"

#include "widgets/choice.h"
#include "widgets/tooltips.h"

#include "audio_clock.h"
#include "editor.h"
#include "editing.h"
#include "editing_convert.h"
#include "keyboard.h"
#include "ardour_ui.h"
#include "gui_thread.h"
#include "actions.h"
#include "region_view.h"
#include "utils.h"
#include "editor_drag.h"
#include "main_clock.h"
#include "ui_config.h"

#include "pbd/i18n.h"

#include "editor_sources.h"

using namespace std;
using namespace ARDOUR;
using namespace ArdourWidgets;
using namespace ARDOUR_UI_UTILS;
using namespace PBD;
using namespace Gtk;
using namespace Glib;
using namespace Editing;
using Gtkmm2ext::Keyboard;

struct ColumnInfo {
	int         index;
	const char* label;
	const char* tooltip;
};

EditorSources::EditorSources (Editor* e)
	: EditorComponent (e)
	, old_focus (0)
	, name_editable (0)
	, _menu (0)
	, ignore_region_list_selection_change (false)
	, ignore_selected_region_change (false)
	, _no_redisplay (false)
	, _sort_type ((Editing::RegionListSortType) 0)
	, _selection (0)
{
	_display.set_size_request (100, -1);
	_display.set_rules_hint (true);
	_display.set_name ("EditGroupList");
	_display.set_fixed_height_mode (true);

	/* Try to prevent single mouse presses from initiating edits.
	   This relies on a hack in gtktreeview.c:gtk_treeview_button_press()
	*/
	_display.set_data ("mouse-edits-require-mod1", (gpointer) 0x1);

	_model = TreeStore::create (_columns);
	_model->set_sort_func (0, sigc::mem_fun (*this, &EditorSources::sorter));
	_model->set_sort_column (0, SORT_ASCENDING);

	/* column widths */
	int bbt_width, check_width, height;

	Glib::RefPtr<Pango::Layout> layout = _display.create_pango_layout (X_("000|000|000"));
	Gtkmm2ext::get_pixel_size (layout, bbt_width, height);

	check_width = 20;

	TreeViewColumn* col_name = manage (new TreeViewColumn ("", _columns.name));
	col_name->set_fixed_width (120);
	col_name->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_position = manage (new TreeViewColumn ("", _columns.position));
	col_position->set_fixed_width (bbt_width);
	col_position->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_end = manage (new TreeViewColumn ("", _columns.end));
	col_end->set_fixed_width (bbt_width);
	col_end->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_length = manage (new TreeViewColumn ("", _columns.length));
	col_length->set_fixed_width (bbt_width);
	col_length->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_sync = manage (new TreeViewColumn ("", _columns.sync));
	col_sync->set_fixed_width (bbt_width);
	col_sync->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_fadein = manage (new TreeViewColumn ("", _columns.fadein));
	col_fadein->set_fixed_width (bbt_width);
	col_fadein->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_fadeout = manage (new TreeViewColumn ("", _columns.fadeout));
	col_fadeout->set_fixed_width (bbt_width);
	col_fadeout->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_locked = manage (new TreeViewColumn ("", _columns.locked));
	col_locked->set_fixed_width (check_width);
	col_locked->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_glued = manage (new TreeViewColumn ("", _columns.glued));
	col_glued->set_fixed_width (check_width);
	col_glued->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_muted = manage (new TreeViewColumn ("", _columns.muted));
	col_muted->set_fixed_width (check_width);
	col_muted->set_sizing (TREE_VIEW_COLUMN_FIXED);
	TreeViewColumn* col_opaque = manage (new TreeViewColumn ("", _columns.opaque));
	col_opaque->set_fixed_width (check_width);
	col_opaque->set_sizing (TREE_VIEW_COLUMN_FIXED);

	_display.append_column (*col_name);
	_display.append_column (*col_position);
	_display.append_column (*col_end);
	_display.append_column (*col_length);
	_display.append_column (*col_sync);
	_display.append_column (*col_fadein);
	_display.append_column (*col_fadeout);
	_display.append_column (*col_locked);
	_display.append_column (*col_glued);
	_display.append_column (*col_muted);
	_display.append_column (*col_opaque);

	TreeViewColumn* col;
	Gtk::Label* l;

	ColumnInfo ci[] = {
		{ 0,   _("Source"),    _("Source name, with number of channels in []'s") },
		{ 1,   _("Position"),  _("Position of start of region") },
		{ 2,   _("End"),       _("Position of end of region") },
		{ 3,   _("Length"),    _("Length of the region") },
		{ 4,   _("Sync"),      _("Position of region sync point, relative to start of the region") },
		{ 5,   _("Fade In"),   _("Length of region fade-in (units: secondary clock), () if disabled") },
		{ 6,   _("Fade Out"),  _("Length of region fade-out (units: secondary clock), () if disabled") },
		{ 7,  S_("Lock|L"),    _("Region position locked?") },
		{ 8,  S_("Gain|G"),    _("Region position glued to Bars|Beats time?") },
		{ 9,  S_("Mute|M"),    _("Region muted?") },
		{ 10, S_("Opaque|O"),  _("Region opaque (blocks regions below it from being heard)?") },
		{ -1, 0, 0 }
	};

	for (int i = 0; ci[i].index >= 0; ++i) {
		col = _display.get_column (ci[i].index);
		l = manage (new Label (ci[i].label));
		set_tooltip (*l, ci[i].tooltip);
		col->set_widget (*l);
		l->show ();

		if (ci[i].index > 6) {
			col->set_expand (false);
			col->set_alignment (ALIGN_CENTER);
		}
	}
	_display.set_model (_model);

	_display.set_headers_visible (true);
	_display.set_rules_hint ();

	/* show path as the row tooltip */
	_display.set_tooltip_column (14); /* path */

	CellRendererText* source_name_cell = dynamic_cast<CellRendererText*>(_display.get_column_cell_renderer (0));
	source_name_cell->property_editable() = true;
	source_name_cell->signal_edited().connect (sigc::mem_fun (*this, &EditorSources::name_edit));
	source_name_cell->signal_editing_started().connect (sigc::mem_fun (*this, &EditorSources::name_editing_started));

	_display.get_selection()->set_select_function (sigc::mem_fun (*this, &EditorSources::selection_filter));

	TreeViewColumn* tv_col = _display.get_column(0);
	CellRendererText* renderer = dynamic_cast<CellRendererText*>(_display.get_column_cell_renderer (0));
	tv_col->add_attribute(renderer->property_text(), _columns.name);
	tv_col->add_attribute(renderer->property_foreground_gdk(), _columns.color_);
	tv_col->set_expand (true);

	CellRendererToggle* locked_cell = dynamic_cast<CellRendererToggle*> (_display.get_column_cell_renderer (7));
	locked_cell->property_activatable() = true;

	TreeViewColumn* locked_col = _display.get_column (7);
	locked_col->add_attribute (locked_cell->property_visible(), _columns.property_toggles_visible);

	CellRendererToggle* glued_cell = dynamic_cast<CellRendererToggle*> (_display.get_column_cell_renderer (8));
	glued_cell->property_activatable() = true;

	TreeViewColumn* glued_col = _display.get_column (8);
	glued_col->add_attribute (glued_cell->property_visible(), _columns.property_toggles_visible);

	CellRendererToggle* muted_cell = dynamic_cast<CellRendererToggle*> (_display.get_column_cell_renderer (9));
	muted_cell->property_activatable() = true;

	TreeViewColumn* muted_col = _display.get_column (9);
	muted_col->add_attribute (muted_cell->property_visible(), _columns.property_toggles_visible);

	CellRendererToggle* opaque_cell = dynamic_cast<CellRendererToggle*> (_display.get_column_cell_renderer (10));
	opaque_cell->property_activatable() = true;

	TreeViewColumn* opaque_col = _display.get_column (10);
	opaque_col->add_attribute (opaque_cell->property_visible(), _columns.property_toggles_visible);

	_display.get_selection()->set_mode (SELECTION_MULTIPLE);
	_display.add_object_drag (_columns.source.index(), "sources");
	_display.set_drag_column (_columns.name.index());

	/* setup DnD handling */

	list<TargetEntry> region_list_target_table;

	region_list_target_table.push_back (TargetEntry ("text/plain"));
	region_list_target_table.push_back (TargetEntry ("text/uri-list"));
	region_list_target_table.push_back (TargetEntry ("application/x-rootwin-drop"));

	_display.add_drop_targets (region_list_target_table);
	_display.signal_drag_data_received().connect (sigc::mem_fun(*this, &EditorSources::drag_data_received));

	_scroller.add (_display);
	_scroller.set_policy (POLICY_AUTOMATIC, POLICY_AUTOMATIC);

	_display.signal_button_press_event().connect (sigc::mem_fun(*this, &EditorSources::button_press), false);
	_change_connection = _display.get_selection()->signal_changed().connect (sigc::mem_fun(*this, &EditorSources::selection_changed));

	_scroller.signal_key_press_event().connect (sigc::mem_fun(*this, &EditorSources::key_press), false);
	_scroller.signal_focus_in_event().connect (sigc::mem_fun (*this, &EditorSources::focus_in), false);
	_scroller.signal_focus_out_event().connect (sigc::mem_fun (*this, &EditorSources::focus_out));

	_display.signal_enter_notify_event().connect (sigc::mem_fun (*this, &EditorSources::enter_notify), false);
	_display.signal_leave_notify_event().connect (sigc::mem_fun (*this, &EditorSources::leave_notify), false);

	// _display.signal_popup_menu().connect (sigc::bind (sigc::mem_fun (*this, &Editor::show__display_context_menu), 1, 0));

	//ARDOUR_UI::instance()->secondary_clock.mode_changed.connect (sigc::mem_fun(*this, &Editor::redisplay_regions));
	ARDOUR_UI::instance()->primary_clock->mode_changed.connect (sigc::mem_fun(*this, &EditorSources::update_all_rows));
//	ARDOUR::Region::RegionPropertyChanged.connect (region_property_connection, MISSING_INVALIDATOR, boost::bind (&EditorSources::region_changed, this, _1, _2), gui_context());

	e->EditorFreeze.connect (editor_freeze_connection, MISSING_INVALIDATOR, boost::bind (&EditorSources::freeze_tree_model, this), gui_context());
	e->EditorThaw.connect (editor_thaw_connection, MISSING_INVALIDATOR, boost::bind (&EditorSources::thaw_tree_model, this), gui_context());
}

bool
EditorSources::focus_in (GdkEventFocus*)
{
	Window* win = dynamic_cast<Window*> (_scroller.get_toplevel ());

	if (win) {
		old_focus = win->get_focus ();
	} else {
		old_focus = 0;
	}

	name_editable = 0;

	/* try to do nothing on focus in (doesn't work, hence selection_count nonsense) */
	return true;
}

bool
EditorSources::focus_out (GdkEventFocus*)
{
	if (old_focus) {
		old_focus->grab_focus ();
		old_focus = 0;
	}

	name_editable = 0;

	return false;
}

bool
EditorSources::enter_notify (GdkEventCrossing*)
{
	if (name_editable) {
		return true;
	}

	/* arm counter so that ::selection_filter() will deny selecting anything for the
	   next two attempts to change selection status.
	*/
	_scroller.grab_focus ();
	Keyboard::magic_widget_grab_focus ();
	return false;
}

bool
EditorSources::leave_notify (GdkEventCrossing*)
{
	if (old_focus) {
		old_focus->grab_focus ();
		old_focus = 0;
	}

	Keyboard::magic_widget_drop_focus ();
	return false;
}

void
EditorSources::set_session (ARDOUR::Session* s)
{
	SessionHandlePtr::set_session (s);
	
	if (s) {
		//get all existing sources
		s->foreach_source (sigc::mem_fun (*this, &EditorSources::add_source));
	
		//register to get new sources that are recorded/imported
		s->SourceAdded.connect (source_added_connection, MISSING_INVALIDATOR, boost::bind (&EditorSources::add_source, this, _1), gui_context());
		s->SourceRemoved.connect (source_removed_connection, MISSING_INVALIDATOR, boost::bind (&EditorSources::remove_source, this, _1), gui_context());
	} else {
		clear();	
	}
	
	//redisplay ();
}

void
EditorSources::remove_source (boost::shared_ptr<ARDOUR::Source> source)
{	TreeModel::iterator i;
	TreeModel::Children rows = _model->children();
	for (i = rows.begin(); i != rows.end(); ++i) {
		boost::shared_ptr<ARDOUR::Source> ss = (*i)[_columns.source];
		if (source == ss) {
			printf("remove a source here\n");  //NOTE:  currently this ONLY happens during remove-last-capture
		}
	}
}

void
EditorSources::add_source (boost::shared_ptr<ARDOUR::Source> source)
{
	if (!source || !_session) {
		return;
	}

	string str;
	Gdk::Color c;

	TreeModel::Row row = *(_model->append());

	bool missing_source = boost::dynamic_pointer_cast<SilentFileSource>(source) != NULL;
	if (missing_source) {
		set_color_from_rgba (c, UIConfiguration::instance().color ("region list missing source"));
	} else {
		set_color_from_rgba (c, UIConfiguration::instance().color ("region list whole file"));
	}

	row[_columns.color_] = c;

	if (source->name()[0] == '/') { // external file

		str = ".../";

		boost::shared_ptr<AudioFileSource> afs = boost::dynamic_pointer_cast<AudioFileSource>(source);
		if (afs) {
			str += source->name();
			str += "[";
			str += afs->n_channels();  //ToDo:   num channels may be its own column?
			str += "]";
		} else {
			str += source->name();
		}

	} else {
		str = source->name();
	}

//	populate_row_name (source, row);
	row[_columns.name] = str;
	row[_columns.source] = source;

	if (missing_source) {
		row[_columns.path] = _("(MISSING) ") + Gtkmm2ext::markup_escape_text (source->name());

	} else {
		boost::shared_ptr<FileSource> fs = boost::dynamic_pointer_cast<FileSource>(source);
		if (fs) {
			row[_columns.path] = Gtkmm2ext::markup_escape_text (fs->path());
		} else {
			row[_columns.path] = Gtkmm2ext::markup_escape_text (source->name());
		}
	}

//	region_row_map.insert(pair<boost::shared_ptr<ARDOUR::Region>, Gtk::TreeModel::RowReference>(source, TreeRowReference(_model, TreePath (row))) );
}


void
EditorSources::remove_unused_regions ()
{
/*	vector<string> choices;
	string prompt;

	if (!_session) {
		return;
	}

	prompt = _("Do you really want to remove unused regions?"
	           "\n(This is destructive and cannot be undone)");

	choices.push_back (_("No, do nothing."));
	choices.push_back (_("Yes, remove."));

	ArdourWidgets::Choice prompter (_("Remove unused regions"), prompt, choices);

	if (prompter.run () == 1) {
		_no_redisplay = true;
		_session->cleanup_regions ();
		_no_redisplay = false;
		redisplay ();
	}
*/
}

void
EditorSources::region_changed (boost::shared_ptr<Region> r, const PropertyChange& what_changed)
{
/*
 * 	//maybe update the grid here
	PropertyChange grid_interests;
	grid_interests.add (ARDOUR::Properties::position);
	grid_interests.add (ARDOUR::Properties::length);
	grid_interests.add (ARDOUR::Properties::sync_position);
	if (what_changed.contains (grid_interests)) {
		_editor->mark_region_boundary_cache_dirty();
	}

	PropertyChange our_interests;

	our_interests.add (ARDOUR::Properties::name);
	our_interests.add (ARDOUR::Properties::position);
	our_interests.add (ARDOUR::Properties::length);
	our_interests.add (ARDOUR::Properties::start);
	our_interests.add (ARDOUR::Properties::sync_position);
	our_interests.add (ARDOUR::Properties::locked);
	our_interests.add (ARDOUR::Properties::position_lock_style);
	our_interests.add (ARDOUR::Properties::muted);
	our_interests.add (ARDOUR::Properties::opaque);
	our_interests.add (ARDOUR::Properties::fade_in);
	our_interests.add (ARDOUR::Properties::fade_out);
	our_interests.add (ARDOUR::Properties::fade_in_active);
	our_interests.add (ARDOUR::Properties::fade_out_active);

	if (what_changed.contains (our_interests)) {
		if (last_row != 0) {

			TreeModel::iterator j = _model->get_iter (last_row.get_path());
			boost::shared_ptr<Region> c = (*j)[_columns.region];

			if (c == r) {
				populate_row (r, (*j), what_changed);

				if (what_changed.contains (ARDOUR::Properties::hidden)) {
					redisplay ();
				}

				return;
			}
		}

		RegionRowMap::iterator it;

		it = region_row_map.find (r);

		if (it != region_row_map.end()){

			TreeModel::iterator j = _model->get_iter ((*it).second.get_path());
			boost::shared_ptr<Region> c = (*j)[_columns.region];

			if (c == r) {
				populate_row (r, (*j), what_changed);

				if (what_changed.contains (ARDOUR::Properties::hidden)) {
					redisplay ();
				}

				return;
			}
		}
	}

	if (what_changed.contains (ARDOUR::Properties::hidden)) {
		redisplay ();
	}
	* 
*/
}

void
EditorSources::selection_changed ()
{
/*
 * 	if (ignore_region_list_selection_change) {
		return;
	}

	_editor->_region_selection_change_updates_region_list = false;

	if (_display.get_selection()->count_selected_rows() > 0) {

		TreeIter iter;
		TreeView::Selection::ListHandle_Path rows = _display.get_selection()->get_selected_rows ();

		_editor->get_selection().clear_regions ();

		for (TreeView::Selection::ListHandle_Path::iterator i = rows.begin(); i != rows.end(); ++i) {

			if ((iter = _model->get_iter (*i))) {

				boost::shared_ptr<Region> region = (*iter)[_columns.region];

				// they could have clicked on a row that is just a placeholder, like "Hidden"
				// although that is not allowed by our selection filter. check it anyway
				// since we need a region ptr.

				if (region) {

					_change_connection.block (true);
					_editor->set_selected_regionview_from_region_list (region, Selection::Add);
					_change_connection.block (false);
				}
			}

		}
	} else {
		_editor->get_selection().clear_regions ();
	}

	_editor->_region_selection_change_updates_region_list = true;
*/
}

void
EditorSources::redisplay ()
{
#if 0
 	if ( _no_redisplay || !_session ) {
		return;
	}

	_display.set_model (Glib::RefPtr<Gtk::TreeStore>(0));
	_model->clear ();
	_model->set_sort_column (-2, SORT_ASCENDING); //Disable sorting to gain performance


	region_row_map.clear();
	parent_regions_sources_map.clear();

	//now add everything we have, via a temporary list used to help with sorting

	const RegionFactory::RegionMap& regions (RegionFactory::regions());

	for (RegionFactory::RegionMap::const_iterator i = regions.begin(); i != regions.end(); ++i) {

		if ( i->second->whole_file()) {
			add_region (i->second);
		}
	}
	
	_model->set_sort_column (0, SORT_ASCENDING); // renabale sorting
	_display.set_model (_model);

	tmp_region_list.clear();

#endif
}

void
EditorSources::update_row (boost::shared_ptr<Region> region)
{
/*	if (!region || !_session) {
		return;
	}

	RegionRowMap::iterator it;

	it = region_row_map.find (region);

	if (it != region_row_map.end()){
		PropertyChange c;
		TreeModel::iterator j = _model->get_iter ((*it).second.get_path());
		populate_row(region, (*j), c);
	}
*/
}

void
EditorSources::update_all_rows ()
{
/*
 * 	if (!_session) {
		return;
	}

	RegionRowMap::iterator i;

	for (i = region_row_map.begin(); i != region_row_map.end(); ++i) {

		TreeModel::iterator j = _model->get_iter ((*i).second.get_path());

		boost::shared_ptr<Region> region = (*j)[_columns.region];
	}
	**/
}

void
EditorSources::format_position (samplepos_t pos, char* buf, size_t bufsize, bool onoff)
{
/*
 * 	Timecode::BBT_Time bbt;
	Timecode::Time timecode;

	if (pos < 0) {
		error << string_compose (_("EditorSources::format_position: negative timecode position: %1"), pos) << endmsg;
		snprintf (buf, bufsize, "invalid");
		return;
	}

	switch (ARDOUR_UI::instance()->primary_clock->mode ()) {
	case AudioClock::BBT:
		bbt = _session->tempo_map().bbt_at_sample (pos);
		if (onoff) {
			snprintf (buf, bufsize, "%03d|%02d|%04d" , bbt.bars, bbt.beats, bbt.ticks);
		} else {
			snprintf (buf, bufsize, "(%03d|%02d|%04d)" , bbt.bars, bbt.beats, bbt.ticks);
		}
		break;

	case AudioClock::MinSec:
		samplepos_t left;
		int hrs;
		int mins;
		float secs;

		left = pos;
		hrs = (int) floor (left / (_session->sample_rate() * 60.0f * 60.0f));
		left -= (samplecnt_t) floor (hrs * _session->sample_rate() * 60.0f * 60.0f);
		mins = (int) floor (left / (_session->sample_rate() * 60.0f));
		left -= (samplecnt_t) floor (mins * _session->sample_rate() * 60.0f);
		secs = left / (float) _session->sample_rate();
		if (onoff) {
			snprintf (buf, bufsize, "%02d:%02d:%06.3f", hrs, mins, secs);
		} else {
			snprintf (buf, bufsize, "(%02d:%02d:%06.3f)", hrs, mins, secs);
		}
		break;

	case AudioClock::Seconds:
		if (onoff) {
			snprintf (buf, bufsize, "%.1f", pos / (float)_session->sample_rate());
		} else {
			snprintf (buf, bufsize, "(%.1f)", pos / (float)_session->sample_rate());
		}
		break;

	case AudioClock::Samples:
		if (onoff) {
			snprintf (buf, bufsize, "%" PRId64, pos);
		} else {
			snprintf (buf, bufsize, "(%" PRId64 ")", pos);
		}
		break;

	case AudioClock::Timecode:
	default:
		_session->timecode_time (pos, timecode);
		if (onoff) {
			snprintf (buf, bufsize, "%02d:%02d:%02d:%02d", timecode.hours, timecode.minutes, timecode.seconds, timecode.frames);
		} else {
			snprintf (buf, bufsize, "(%02d:%02d:%02d:%02d)", timecode.hours, timecode.minutes, timecode.seconds, timecode.frames);
		}
		break;
	}
*/
}

void
EditorSources::populate_row (boost::shared_ptr<Region> region, TreeModel::Row const &row, PBD::PropertyChange const &what_changed)
{
/*
 * 	boost::shared_ptr<AudioRegion> audioregion = boost::dynamic_pointer_cast<AudioRegion>(region);
	//uint32_t used = _session->playlists->region_use_count (region);
	uint32_t used = 1;

	PropertyChange c;
	const bool all = what_changed == c;

	if (all || what_changed.contains (Properties::position)) {
		populate_row_position (region, row, used);
	}
	if (all || what_changed.contains (Properties::start) || what_changed.contains (Properties::sync_position)) {
		populate_row_sync (region, row, used);
	}
	if (all || what_changed.contains (Properties::fade_in)) {
		populate_row_fade_in (region, row, used, audioregion);
	}
	if (all || what_changed.contains (Properties::fade_out)) {
		populate_row_fade_out (region, row, used, audioregion);
	}
	if (all || what_changed.contains (Properties::locked)) {
		populate_row_locked (region, row, used);
	}
	if (all || what_changed.contains (Properties::position_lock_style)) {
		populate_row_glued (region, row, used);
	}
	if (all || what_changed.contains (Properties::muted)) {
		populate_row_muted (region, row, used);
	}
	if (all || what_changed.contains (Properties::opaque)) {
		populate_row_opaque (region, row, used);
	}
	if (all || what_changed.contains (Properties::length)) {
		populate_row_end (region, row, used);
		populate_row_length (region, row);
	}
	if (all) {
		populate_row_source (region, row);
	}
	if (all || what_changed.contains (Properties::name)) {
		populate_row_name (region, row);
	}
	if (all) {
		populate_row_used (region, row, used);
	}
*/
}

#if 0
	if (audioRegion && fades_in_seconds) {

		samplepos_t left;
		int mins;
		int millisecs;

		left = audioRegion->fade_in()->back()->when;
		mins = (int) floor (left / (_session->sample_rate() * 60.0f));
		left -= (samplepos_t) floor (mins * _session->sample_rate() * 60.0f);
		millisecs = (int) floor ((left * 1000.0f) / _session->sample_rate());

		if (audioRegion->fade_in()->back()->when >= _session->sample_rate()) {
			sprintf (fadein_str, "%01dM %01dmS", mins, millisecs);
		} else {
			sprintf (fadein_str, "%01dmS", millisecs);
		}

		left = audioRegion->fade_out()->back()->when;
		mins = (int) floor (left / (_session->sample_rate() * 60.0f));
		left -= (samplepos_t) floor (mins * _session->sample_rate() * 60.0f);
		millisecs = (int) floor ((left * 1000.0f) / _session->sample_rate());

		if (audioRegion->fade_out()->back()->when >= _session->sample_rate()) {
			sprintf (fadeout_str, "%01dM %01dmS", mins, millisecs);
		} else {
			sprintf (fadeout_str, "%01dmS", millisecs);
		}
	}
#endif

void
EditorSources::populate_row_used (boost::shared_ptr<Region>, TreeModel::Row const& row, uint32_t used)
{
/*	char buf[8];
	snprintf (buf, sizeof (buf), "%4d" , used);
	row[_columns.used] = buf;
*/
}

void
EditorSources::populate_row_name (boost::shared_ptr<Region> region, TreeModel::Row const &row)
{
/*	if (region->n_channels() > 1) {
		row[_columns.name] = string_compose("%1  [%2]", Gtkmm2ext::markup_escape_text (region->name()), region->n_channels());
	} else {
		row[_columns.name] = Gtkmm2ext::markup_escape_text (region->name());
	}
*/
}

void
EditorSources::populate_row_source (boost::shared_ptr<Region> region, TreeModel::Row const &row)
{
/*	if (boost::dynamic_pointer_cast<SilentFileSource>(region->source())) {
		row[_columns.path] = _("MISSING ") + Gtkmm2ext::markup_escape_text (region->source()->name());
	} else {
		row[_columns.path] = Gtkmm2ext::markup_escape_text (region->source()->name());
	}
*/
}

void
EditorSources::show_context_menu (int button, int time)
{

}

bool
EditorSources::key_press (GdkEventKey* ev)
{

}

bool
EditorSources::button_press (GdkEventButton *ev)
{
}

int
EditorSources::sorter (TreeModel::iterator a, TreeModel::iterator b)
{

}

void
EditorSources::reset_sort_type (RegionListSortType type, bool force)
{

}

void
EditorSources::reset_sort_direction (bool up)
{
}

void
EditorSources::selection_mapover (sigc::slot<void,boost::shared_ptr<Region> > sl)
{

}


void
EditorSources::drag_data_received (const RefPtr<Gdk::DragContext>& context,
                                   int x, int y,
                                   const SelectionData& data,
                                   guint info, guint time)
{

}

bool
EditorSources::selection_filter (const RefPtr<TreeModel>& model, const TreeModel::Path& path, bool already_selected)
{

}

void
EditorSources::name_editing_started (CellEditable* ce, const Glib::ustring& path)
{

}

void
EditorSources::name_edit (const std::string& path, const std::string& new_text)
{

}

/** @return Region that has been dragged out of the list, or 0 */
boost::shared_ptr<Region>
EditorSources::get_dragged_region ()
{

}

void
EditorSources::clear ()
{
	_display.set_model (Glib::RefPtr<Gtk::TreeStore> (0));
	_model->clear ();
	_display.set_model (_model);
}

boost::shared_ptr<Region>
EditorSources::get_single_selection ()
{

}

void
EditorSources::freeze_tree_model ()
{
	_display.set_model (Glib::RefPtr<Gtk::TreeStore>(0));
	_model->set_sort_column (-2, SORT_ASCENDING); //Disable sorting to gain performance
}

void
EditorSources::thaw_tree_model (){

	_model->set_sort_column (0, SORT_ASCENDING); // renabale sorting
	_display.set_model (_model);
}

XMLNode &
EditorSources::get_state () const
{
	XMLNode* node = new XMLNode (X_("SourcesList"));

	node->set_property (X_("sort-type"), _sort_type);

	RefPtr<Action> act = ActionManager::get_action (X_("SourcesList"), X_("SortAscending"));
	bool const ascending = RefPtr<RadioAction>::cast_dynamic(act)->get_active ();
	node->set_property (X_("sort-ascending"), ascending);

	return *node;
}

void
EditorSources::set_state (const XMLNode & node)
{
	bool changed = false;

	if (node.name() != X_("SourcesList")) {
		return;
	}

	Editing::RegionListSortType t;
	if (node.get_property (X_("sort-type"), t)) {

		if (_sort_type != t) {
			changed = true;
		}

		reset_sort_type (t, true);
		RefPtr<RadioAction> ract = sort_type_action (t);
		ract->set_active ();
	}

	bool yn;
	if (node.get_property (X_("sort-ascending"), yn)) {
		SortType old_sort_type;
		int old_sort_column;

		_model->get_sort_column_id (old_sort_column, old_sort_type);

		if (old_sort_type != (yn ? SORT_ASCENDING : SORT_DESCENDING)) {
			changed = true;
		}

		reset_sort_direction (yn);
		RefPtr<Action> act;

		if (yn) {
			act = ActionManager::get_action (X_("SourcesList"), X_("SortAscending"));
		} else {
			act = ActionManager::get_action (X_("SourcesList"), X_("SortDescending"));
		}

		RefPtr<RadioAction>::cast_dynamic(act)->set_active ();
	}

	if (changed) {
		redisplay ();
	}
}

RefPtr<RadioAction>
EditorSources::sort_type_action (Editing::RegionListSortType t) const
{
	const char* action = 0;

	switch (t) {
	case Editing::ByName:
		action = X_("SortByRegionName");
		break;
	case Editing::ByLength:
		action = X_("SortByRegionLength");
		break;
	case Editing::ByPosition:
		action = X_("SortByRegionPosition");
		break;
	case Editing::ByTimestamp:
		action = X_("SortByRegionTimestamp");
		break;
	case Editing::ByStartInFile:
		action = X_("SortByRegionStartinFile");
		break;
	case Editing::ByEndInFile:
		action = X_("SortByRegionEndinFile");
		break;
	case Editing::BySourceFileName:
		action = X_("SortBySourceFileName");
		break;
	case Editing::BySourceFileLength:
		action = X_("SortBySourceFileLength");
		break;
	case Editing::BySourceFileCreationDate:
		action = X_("SortBySourceFileCreationDate");
		break;
	case Editing::BySourceFileFS:
		action = X_("SortBySourceFilesystem");
		break;
	default:
		fatal << string_compose (_("programming error: %1: %2"), "EditorSources: impossible sort type", (int) t) << endmsg;
		abort(); /*NOTREACHED*/
	}

	RefPtr<Action> act = ActionManager::get_action (X_("RegionList"), action);
	assert (act);

	return RefPtr<RadioAction>::cast_dynamic (act);
}

RefPtr<Action>
EditorSources::hide_action () const
{
	return ActionManager::get_action (X_("SourcesList"), X_("rlHide"));

}

RefPtr<Action>
EditorSources::show_action () const
{
	return ActionManager::get_action (X_("SourcesList"), X_("rlShow"));
}

RefPtr<Action>
EditorSources::remove_unused_regions_action () const
{
	return ActionManager::get_action (X_("SourcesList"), X_("removeUnusedRegions"));
}
