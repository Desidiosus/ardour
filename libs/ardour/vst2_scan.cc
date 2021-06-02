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

#include <iostream>

#include "pbd/gstdio_compat.h"
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#ifdef COMPILER_MSVC
#include <sys/utime.h>
#else
#include <utime.h>
#endif

#ifndef PLATFORM_WINDOWS
#include <sys/utsname.h>
#endif


#include "pbd/basename.h"
#include "pbd/error.h"
#include "pbd/failed_constructor.h"

#include "ardour/filesystem_paths.h"
#include "ardour/linux_vst_support.h"
#include "ardour/mac_vst_support.h"
#include "ardour/vst_types.h"
#include "ardour/vst2_scan.h"

#ifdef WINDOWS_VST_SUPPORT
#include <fst.h>
#endif

#include "pbd/i18n.h"

using namespace std;

/* ID for shell plugins */
static int vstfx_current_loading_id = 0;

/* ****************************************************************************
 * VST system-under-test methods
 */

static
bool vstfx_midi_input (VSTState* vstfx)
{
	AEffect* plugin = vstfx->plugin;

	if ((plugin->flags & effFlagsIsSynth)
			|| (plugin->dispatcher (plugin, effCanDo, 0, 0, const_cast<char*> ("receiveVstEvents"), 0.0f) > 0)
			|| (plugin->dispatcher (plugin, effCanDo, 0, 0, const_cast<char*> ("receiveVstMidiEvent"), 0.0f) > 0)
			|| (plugin->dispatcher (plugin, effCanDo, 0, 0, const_cast<char*> ("receiveVstMidiEvents"), 0.0f) > 0)
		 ) {
		return true;
	}

	return false;
}

static
bool vstfx_midi_output (VSTState* vstfx)
{
	AEffect* plugin = vstfx->plugin;

	int const vst_version = plugin->dispatcher (plugin, effGetVstVersion, 0, 0, 0, 0.0f);

	if (vst_version >= 2) {

		if (   (plugin->dispatcher (plugin, effCanDo, 0, 0, const_cast<char*> ("sendVstEvents"), 0.0f) > 0)
		    || (plugin->dispatcher (plugin, effCanDo, 0, 0, const_cast<char*> ("sendVstMidiEvent"), 0.0f) > 0)
		    || (plugin->dispatcher (plugin, effCanDo, 0, 0, const_cast<char*> ("sendVstMidiEvents"), 0.0f) > 0)
		   ) {
			return true;
		}
	}

	return false;
}

/** simple 'dummy' audiomaster callback to instantiate the plugin
 * and query information
 */
static intptr_t
simple_master_callback (AEffect *, int32_t opcode, int32_t, intptr_t, void *ptr, float)
{
	const char* vstfx_can_do_strings[] = {
		"supplyIdle",
		"sendVstTimeInfo",
		"sendVstEvents",
		"sendVstMidiEvent",
		"receiveVstEvents",
		"receiveVstMidiEvent",
		"supportShell",
		"shellCategory",
		"shellCategorycurID",
		"sizeWindow"
	};
	const int vstfx_can_do_string_count = 9;

	if (opcode == audioMasterVersion) {
		return 2400;
	}
	else if (opcode == audioMasterCanDo) {
		for (int i = 0; i < vstfx_can_do_string_count; i++) {
			if (! strcmp (vstfx_can_do_strings[i], (const char*)ptr)) {
				return 1;
			}
		}
		return 0;
	}
	else if (opcode == audioMasterCurrentId) {
		return vstfx_current_loading_id;
	}
	else {
		return 0;
	}
}


/** main plugin query and test function */
static VSTInfo*
vstfx_parse_vst_state (VSTState* vstfx)
{
	assert (vstfx);

	VSTInfo* info = (VSTInfo*) malloc (sizeof (VSTInfo));
	if (!info) {
		return 0;
	}

	/* We need to init the creator because some plugins
	 * fail to implement getVendorString, and so won't stuff the
	 * string with any name */

	char creator[65] = "Unknown";
	char name[65] = "";

	AEffect* plugin = vstfx->plugin;


	plugin->dispatcher (plugin, effGetEffectName, 0, 0, name, 0);

	if (strlen (name) == 0) {
		plugin->dispatcher (plugin, effGetProductString, 0, 0, name, 0);
	}

	if (strlen (name) == 0) {
		info->name = strdup (vstfx->handle->name);
	} else {
		info->name = strdup (name);
	}

	/*If the plugin doesn't bother to implement GetVendorString we will
	 * have pre-stuffed the string with 'Unknown' */

	plugin->dispatcher (plugin, effGetVendorString, 0, 0, creator, 0);

	/* Some plugins DO implement GetVendorString, but DON'T put a name in it
	 * so if its just a zero length string we replace it with 'Unknown' */

	if (strlen (creator) == 0) {
		info->creator = strdup ("Unknown");
	} else {
		info->creator = strdup (creator);
	}


	switch (plugin->dispatcher (plugin, effGetPlugCategory, 0, 0, 0, 0))
	{
		case kPlugCategEffect:         info->Category = strdup ("Effect"); break;
		case kPlugCategSynth:          info->Category = strdup ("Instrument"); break;
		case kPlugCategAnalysis:       info->Category = strdup ("Analyser"); break;
		case kPlugCategMastering:      info->Category = strdup ("Mastering"); break;
		case kPlugCategSpacializer:    info->Category = strdup ("Spatial"); break;
		case kPlugCategRoomFx:         info->Category = strdup ("RoomFx"); break;
		case kPlugSurroundFx:          info->Category = strdup ("SurroundFx"); break;
		case kPlugCategRestoration:    info->Category = strdup ("Restoration"); break;
		case kPlugCategOfflineProcess: info->Category = strdup ("Offline"); break;
		case kPlugCategShell:          info->Category = strdup ("Shell"); break;
		case kPlugCategGenerator:      info->Category = strdup ("Generator"); break;
		default:                       info->Category = strdup ("Unknown"); break;
	}

	info->UniqueID = plugin->uniqueID;

	info->numInputs = plugin->numInputs;
	info->numOutputs = plugin->numOutputs;
	info->numParams = plugin->numParams;
	info->wantMidi = (vstfx_midi_input (vstfx) ? 1 : 0) | (vstfx_midi_output (vstfx) ? 2 : 0);
	info->hasEditor = plugin->flags & effFlagsHasEditor ? true : false;
	info->isInstrument = (plugin->flags & effFlagsIsSynth) ? 1 : 0;
	info->canProcessReplacing = plugin->flags & effFlagsCanReplacing ? true : false;
	info->ParamNames = (char **) malloc (sizeof (char*)*info->numParams);
	info->ParamLabels = (char **) malloc (sizeof (char*)*info->numParams);

#ifdef __APPLE__
	if (info->hasEditor) {
		/* we only support Cocoa UIs (just like Reaper) */
		info->hasEditor = (plugin->dispatcher (plugin, effCanDo, 0, 0, const_cast<char*> ("hasCockosViewAsConfig"), 0.0f) & 0xffff0000) == 0xbeef0000;
	}
#endif

	for (int i = 0; i < info->numParams; ++i) {
		char name[VestigeMaxLabelLen];
		char label[VestigeMaxLabelLen];

		/* Not all plugins give parameters labels as well as names */

		strcpy (name, "No Name");
		strcpy (label, "No Label");

		plugin->dispatcher (plugin, effGetParamName, i, 0, name, 0);
		info->ParamNames[i] = strdup (name);

		//NOTE: 'effGetParamLabel' is no longer defined in vestige headers
		//plugin->dispatcher (plugin, effGetParamLabel, i, 0, label, 0);
		info->ParamLabels[i] = strdup (label);
	}
	return info;
}


static bool vstfx_instantiate_and_get_info (const char* dllpath, ARDOUR::PluginType type, std::vector<VSTInfo*> *infos, int uniqueID);

/** wrapper around \ref vstfx_parse_vst_state,
 * iterate over plugins in shell, translate VST-info into ardour VSTState
 */
static bool
vstfx_info_from_plugin (const char *dllpath, VSTState* vstfx, vector<VSTInfo *> *infos, enum ARDOUR::PluginType type)
{
	assert (vstfx);
	VSTInfo *info;

	if (!(info = vstfx_parse_vst_state (vstfx))) {
		return false;
	}

	infos->push_back (info);
#if 1 // shell-plugin support
	/* If this plugin is a Shell and we are not already inside a shell plugin
	 * read the info for all of the plugins contained in this shell.
	 */
	if (!strncmp (info->Category, "Shell", 5)
			&& vstfx->handle->plugincnt == 1) {
		int id;
		vector< pair<int, string> > ids;
		AEffect *plugin = vstfx->plugin;

		do {
			char name[65] = "Unknown";
			id = plugin->dispatcher (plugin, effShellGetNextPlugin, 0, 0, name, 0);
			ids.push_back (std::make_pair (id, name));
		} while ( id != 0 );

		switch (type) {
#ifdef WINDOWS_VST_SUPPORT
			case ARDOUR::Windows_VST:
				fst_close (vstfx);
				break;
#endif
#ifdef LXVST_SUPPORT
			case ARDOUR::LXVST:
				vstfx_close (vstfx);
				break;
#endif
#ifdef MACVST_SUPPORT
			case ARDOUR::MacVST:
				mac_vst_close (vstfx);
				break;
#endif
			default:
				assert (0);
				break;
		}

		for (vector< pair<int, string> >::iterator x = ids.begin (); x != ids.end (); ++x) {
			id = (*x).first;
			if (id == 0) continue;
			/* recurse vstfx_get_info() */

			bool ok = vstfx_instantiate_and_get_info (dllpath, type, infos, id);
			if (ok) {
				// One shell (some?, all?) does not report the actual plugin name
				// even after the shelled plugin has been instantiated.
				// Replace the name of the shell with the real name.
				info = infos->back ();
				free (info->name);

				if ((*x).second.length () == 0) {
					info->name = strdup ("Unknown");
				}
				else {
					info->name = strdup ((*x).second.c_str ());
				}
			}
		}
	} else {
		switch (type) {
#ifdef WINDOWS_VST_SUPPORT
			case ARDOUR::Windows_VST:
				fst_close (vstfx);
				break;
#endif
#ifdef LXVST_SUPPORT
			case ARDOUR::LXVST:
				vstfx_close (vstfx);
				break;
#endif
#ifdef MACVST_SUPPORT
			case ARDOUR::MacVST:
				mac_vst_close (vstfx);
				break;
#endif
			default:
				assert (0);
				break;
		}
	}
#endif
	return true;
}

static bool
vstfx_instantiate_and_get_info (const char* dllpath, ARDOUR::PluginType type, std::vector<VSTInfo*> *infos, int uniqueID)
{
	VSTHandle* h     = NULL;
	VSTState*  vstfx = NULL;

	switch (type) {
#ifdef WINDOWS_VST_SUPPORT
		case ARDOUR::Windows_VST:
			h = fst_load (dllpath);
			break;
#endif
#ifdef LXVST_SUPPORT
		case ARDOUR::LXVST:
			h = vstfx_load (dllpath);
			break;
#endif
#ifdef MACVST_SUPPORT
		case ARDOUR::MacVST:
			h = mac_vst_load (dllpath);
			break;
#endif
		default:
			break;
	}

	if (!h) {
		PBD::warning << string_compose (_("Cannot get load VST pluginfrom '%1'"), dllpath) << endmsg;
		return false;
	}

	vstfx_current_loading_id = uniqueID;

	switch (type) {
#ifdef WINDOWS_VST_SUPPORT
		case ARDOUR::Windows_VST:
			if (!(vstfx = fst_instantiate (h, simple_master_callback, 0))) {
				fst_unload (&h);
			}
			break;
#endif
#ifdef LXVST_SUPPORT
		case ARDOUR::LXVST:
			if (!(vstfx = vstfx_instantiate (h, simple_master_callback, 0))) {
				vstfx_unload (h);
			}
			break;
#endif
#ifdef MACVST_SUPPORT
		case ARDOUR::MacVST:
			if (!(vstfx = mac_vst_instantiate (h, simple_master_callback, 0))) {
				mac_vst_unload (h);
			}
			break;
#endif
		default:
			break;
	}

	vstfx_current_loading_id = 0;

	if (!vstfx) {
		PBD::warning << string_compose (_("Cannot get VST information from '%1': instantiation failed."), dllpath) << endmsg;
		return false;
	}

	return vstfx_info_from_plugin (dllpath, vstfx, infos, type);
}

static bool
discover_vst2 (std::string const& path, ARDOUR::PluginType type, std::vector<ARDOUR::VST2Info>& rv, bool verbose)
{
	bool ok = false;
#if 0
	switch (type) {
#ifdef WINDOWS_VST_SUPPORT
		case ARDOUR::Windows_VST:
			ok = vstfx_instantiate_and_get_info_fst (dllpath, infos, 0);
			break;
#endif
#ifdef LXVST_SUPPORT
		case ARDOUR::LXVST:
			ok = vstfx_instantiate_and_get_info_lx (dllpath, infos, 0);
			break;
#endif
#ifdef MACVST_SUPPORT
		case ARDOUR::MacVST:
			ok = vstfx_instantiate_and_get_info_mac (dllpath, infos, 0);
			break;
#endif
		default:
			return false;
			break;
	}
#endif

	return ok;
}

static std::string vst2_suffix () {
#ifdef __APPLE__
	return "";
#elif defined PLATFORM_WINDOWS
	return ".dll";
#else // Linux
	return ".so";
#endif
}

static string
vst2_info_cache_dir ()
{
	string dir = Glib::build_filename (ARDOUR::user_cache_directory (), "vst");
	/* if the directory doesn't exist, try to create it */
	if (!Glib::file_test (dir, Glib::FILE_TEST_IS_DIR)) {
		if (g_mkdir (dir.c_str (), 0700)) {
			PBD::fatal << "Cannot create VST info folder '" << dir << "'" << endmsg;
		}
	}
	return dir;
}

#include "sha1.c"

string
ARDOUR::vst2_cache_file (std::string const& path)
{
	char hash[41];
	Sha1Digest s;
	sha1_init (&s);
	sha1_write (&s, (const uint8_t *) path.c_str(), path.size());
	sha1_result_hash (&s, hash);
	return Glib::build_filename (vst2_info_cache_dir (), std::string (hash) + std::string (".v2i"));
}

string
ARDOUR::vst2_valid_cache_file (std::string const& path, bool verbose)
{
	string const cache_file = ARDOUR::vst2_cache_file (path);
	if (!Glib::file_test (cache_file, Glib::FileTest (Glib::FILE_TEST_EXISTS | Glib::FILE_TEST_IS_REGULAR))) {
		return "";
	}

	if (verbose) {
		PBD::info << "Found cache file: '" << cache_file <<"'" << endmsg;
	}

	GStatBuf sb_vst;
	GStatBuf sb_v3i;

	if (g_stat (path.c_str(), &sb_vst) == 0 && g_stat (cache_file.c_str (), &sb_v3i) == 0) {
		if (sb_vst.st_mtime < sb_v3i.st_mtime) {
			/* plugin is older than cache file */
			if (verbose) {
				PBD::info << "Cache file is up-to-date." << endmsg;
			}
			return cache_file;
		} else if  (verbose) {
			PBD::info << "Stale cache." << endmsg;
		}
	}
	return "";
}

static void
touch_cachefile (std::string const& path, std::string const& cache_file)
{
	GStatBuf sb_vst;
	GStatBuf sb_v3i;
	if (g_stat (path.c_str(), &sb_vst) == 0 && g_stat (cache_file.c_str (), &sb_v3i) == 0) {
		struct utimbuf utb;
		utb.actime = sb_v3i.st_atime;
		utb.modtime = std::max (sb_vst.st_mtime, sb_v3i.st_mtime);
		g_utime (cache_file.c_str (), &utb);
	}
}

static bool
vst2_save_cache_file (std::string const& path, XMLNode* root, bool verbose)
{
	string const cache_file = ARDOUR::vst2_cache_file (path);

	XMLTree tree;
	tree.set_root (root);
	if (!tree.write (cache_file)) {
		PBD::error << "Could not save VST2 plugin cache to: " << cache_file << endmsg;
		return false;
	} else {
		touch_cachefile (path, cache_file);
	}
	if (verbose) {
		root->dump (std::cout, "\t");
	}
	return true;
}

bool
ARDOUR::vst2_scan_and_cache (std::string const& path, ARDOUR::PluginType type, boost::function<void (std::string const&, VST2Info const&)> cb, bool verbose)
{
	XMLNode* root = new XMLNode ("VST2Cache");
	root->set_property ("version", 1);
	root->set_property ("binary", path);
	root->set_property ("arch", ""); // dll_info()

	try {
		std::vector<VST2Info> nfo;
		if (!discover_vst2 (path, type, nfo, verbose)) {
			delete root;
			return false;
		}
		if (nfo.empty ()) {
			cerr << "No plugins in VST2 plugin: '" << path << "'\n";
			delete root;
			return false;
		}
		for (std::vector<VST2Info>::const_iterator i = nfo.begin(); i != nfo.end(); ++i) {
			cb (path, *i);
			root->add_child_nocopy (i->state ());
		}
	} catch (...) {
		cerr << "Cannot load VST2 plugin: '" << path << "'\n";
		delete root;
		return false;
	}

	return vst2_save_cache_file (path, root, verbose);
}


using namespace ARDOUR;

VST2Info::VST2Info (XMLNode const& node)
	: id (0)
	, n_inputs (0)
	, n_outputs (0)
	, has_midi_input (false)
	, can_process_replace (false)
	, has_editor (false)
{
	bool err = false;

	if (node.name() != "VST2Info") {
		throw failed_constructor ();
	}
	err |= !node.get_property ("id", id);
	err |= !node.get_property ("name", name);
	err |= !node.get_property ("creator", creator);
	err |= !node.get_property ("category", category);
	err |= !node.get_property ("version", version);

	err |= !node.get_property ("n_inputs", n_inputs);
	err |= !node.get_property ("n_outputs", n_outputs);
	err |= !node.get_property ("has_midi_input", has_midi_input);
	err |= !node.get_property ("can_process_replace", can_process_replace);
	err |= !node.get_property ("has_editor", has_editor);

	if (err) {
		throw failed_constructor ();
	}
}

XMLNode&
VST2Info::state () const
{
	XMLNode* node = new XMLNode("VST2Info");
	node->set_property ("id",       id);
	node->set_property ("name",     name);
	node->set_property ("creator",  creator);
	node->set_property ("category", category);
	node->set_property ("version",  version);

	node->set_property ("n_inputs",             n_inputs);
	node->set_property ("n_outputs",            n_outputs);
	node->set_property ("has_midi_input",       has_midi_input);
	node->set_property ("can_process_replace",  can_process_replace);
	node->set_property ("has_editor",           has_editor);
	return *node;
}

