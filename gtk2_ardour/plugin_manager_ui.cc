/*
 * Copyright (C) 2021 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include "ardour/types_convert.h"

#include "gtkmm2ext/gui_thread.h"

#include "plugin_manager_ui.h"

#include "pbd/i18n.h"

using namespace ARDOUR;

PluginManagerUI::PluginManagerUI ()
	: ArdourWindow (_("Plugin Manager"))
{
	//plugin_model = Gtk::TreeStore::create (plugin_columns);
	plugin_model = Gtk::ListStore::create (plugin_columns);

	plugin_display.append_column (_("Status"), plugin_columns.status);
	plugin_display.append_column (_("Name"), plugin_columns.name);
	plugin_display.append_column (_("Creator"), plugin_columns.creator);
	plugin_display.append_column (_("Type"), plugin_columns.type);
	plugin_display.append_column (_("Path"), plugin_columns.path);

	plugin_display.set_model (plugin_model);
	plugin_display.set_headers_visible (true);
	plugin_display.set_headers_clickable (true);
	plugin_display.set_reorderable (false);
	plugin_display.set_rules_hint (true);

	plugin_model->set_sort_column (plugin_columns.name.index(), Gtk::SORT_ASCENDING);
	plugin_display.set_name("PluginSelectorDisplay");
#if 0
	plugin_display.signal_row_activated().connect_notify (sigc::mem_fun(*this, &PluginManagerUI::row_activated));
	plugin_display.get_selection()->signal_changed().connect (sigc::mem_fun(*this, &PluginManagerUI::display_selection_changed));
#endif

	_scroller.add (plugin_display);
	_scroller.set_policy (Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

	add (_scroller);
	_scroller.show_all ();

	PluginManager::instance ().PluginListChanged.connect (_manager_connection, invalidator (*this), boost::bind (&PluginManagerUI::refill, this), gui_context());
}

PluginManagerUI::~PluginManagerUI ()
{
}

void
PluginManagerUI::on_show ()
{
	refill ();
	ArdourWindow::on_show ();
}

void
PluginManagerUI::refill ()
{
	std::vector<PluginScanLogEntry> psl;
	PluginManager& mgr (PluginManager::instance ());
	mgr.scan_log (psl);

	plugin_display.set_model (Glib::RefPtr<Gtk::TreeStore>(0));
	plugin_model->clear ();

	for (std::vector<PluginScanLogEntry>::const_iterator i = psl.begin(); i != psl.end(); ++i) {
		PluginInfoList const& plugs = i->nfo ();
		if (plugs.size () == 0) {
			Gtk::TreeModel::Row newrow = *(plugin_model->append());
			newrow[plugin_columns.path] = i->path ();
			newrow[plugin_columns.type] = enum_2_string (i->type ());
			newrow[plugin_columns.name] = "-";
		} else if (plugs.size () == 1) {
			Gtk::TreeModel::Row newrow = *(plugin_model->append());
			newrow[plugin_columns.path] = i->path ();
			newrow[plugin_columns.path] = i->path ();
			newrow[plugin_columns.type] = enum_2_string (i->type ());
			newrow[plugin_columns.name] = plugs.front()->name;
		} else {
			printf ("SHELL, no oil\n");
		}
	}
	plugin_display.set_model (plugin_model);
}
