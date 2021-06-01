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

//#include "vst3/vst3.h"

#include "pbd/basename.h"
#include "pbd/error.h"
#include "pbd/failed_constructor.h"

#include "ardour/filesystem_paths.h"
//#include "ardour/vst3_module.h"
//#include "ardour/vst3_host.h"
#include "ardour/vst2_scan.h"

using namespace std;

static bool
discover_vst2 (/*boost::shared_ptr<ARDOUR::VST3PluginModule> m, */std::vector<ARDOUR::VST2Info>& rv, bool verbose)
{
	return true;
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
ARDOUR::vst2_scan_and_cache (std::string const& path, ARDOUR::PluginType, boost::function<void (std::string const&, VST2Info const&)> cb, bool verbose)
{
	XMLNode* root = new XMLNode ("VST2Cache");
	root->set_property ("version", 1);
	root->set_property ("binary", path);
	root->set_property ("arch", ""); // dll_info()

	try {
#if 0
		boost::shared_ptr<VST3PluginModule> m = VST3PluginModule::load (path); 
		std::vector<VST2Info> nfo;
		if (!discover_vst2 (m, nfo, verbose)) {
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
#endif
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

