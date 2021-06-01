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

#ifndef _ardour_vst2_scan_h_
#define _ardour_vst2_scan_h_

#include <string>
#include <vector>
#include <boost/shared_ptr.hpp>

#include "pbd/xml++.h"

#include "ardour/libardour_visibility.h"
#include "ardour/types.h"

namespace ARDOUR {

struct VST2Info {
	VST2Info ()
		: id (0)
		, n_inputs (0)
		, n_outputs (0)
		, has_midi_input (false)
		, can_process_replace (false)
		, has_editor (false)
	{}

	VST2Info (XMLNode const&);
	XMLNode& state () const;

	int32_t     id;
	std::string name;
	std::string creator; // vendor;
	std::string category;
	std::string version;

	int n_inputs;
	int n_outputs;

	bool has_midi_input;
	bool can_process_replace;
	bool has_editor;
};

LIBARDOUR_API extern std::string
vst2_cache_file (std::string const& path);

LIBARDOUR_API extern std::string
vst2_valid_cache_file (std::string const& path, bool verbose = false);

LIBARDOUR_API extern bool
vst2_scan_and_cache (std::string const& path, ARDOUR::PluginType, boost::function<void (std::string const&, VST2Info const&)> cb, bool verbose = false);

} // namespace ARDOUR

#endif

