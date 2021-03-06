/*

Copyright (c) 2015, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include <cstdint>

#include "libtorrent/bdecode.hpp"
#include "libtorrent/read_resume_data.hpp"
#include "libtorrent/add_torrent_params.hpp"
#include "libtorrent/socket_io.hpp" // for read_*_endpoint()
#include "libtorrent/hasher.hpp"
#include "libtorrent/torrent_info.hpp"

namespace libtorrent
{
	namespace
	{
		void apply_flag(std::uint64_t& current_flags
			, bdecode_node const& n
			, char const* name
			, std::uint64_t const flag)
		{
			if (n.dict_find_int_value(name, 0) == 0)
			{
				current_flags &= ~flag;
			}
			else
			{
				current_flags |= flag;
			}
		}
	}

	add_torrent_params read_resume_data(bdecode_node const& rd, error_code& ec)
	{
		add_torrent_params ret;
		if (bdecode_node alloc = rd.dict_find_string("allocation"))
		{
			ret.storage_mode = (alloc.string_value() == "allocate"
				|| alloc.string_value() == "full")
				? storage_mode_allocate : storage_mode_sparse;
		}

		if (rd.dict_find_string_value("file-format")
			!= "libtorrent resume file")
		{
			ec = errors::invalid_file_tag;
			return ret;
		}

		auto info_hash = rd.dict_find_string_value("info-hash");
		if (info_hash.size() != 20)
		{
			ec = errors::missing_info_hash;
			return ret;
		}

		ret.name = rd.dict_find_string_value("name").to_string();

		ret.info_hash.assign(info_hash.data());

		// TODO: 4 add unit test for this, and all other fields of the resume data
		bdecode_node info = rd.dict_find_dict("info");
		if (info)
		{
			// verify the info-hash of the metadata stored in the resume file matches
			// the torrent we're loading
			sha1_hash const resume_ih = hasher(info.data_section()).final();

			// if url is set, the info_hash is not actually the info-hash of the
			// torrent, but the hash of the URL, until we have the full torrent
			// only require the info-hash to match if we actually passed in one
			if (resume_ih == ret.info_hash)
			{
				ret.ti = std::make_shared<torrent_info>(resume_ih);

				error_code err;
				if (!ret.ti->parse_info_section(info, err, 0))
				{
					ec = err;
				}
			}
		}

		ret.total_uploaded = rd.dict_find_int_value("total_uploaded");
		ret.total_downloaded = rd.dict_find_int_value("total_downloaded");

		ret.active_time = int(rd.dict_find_int_value("active_time"));
		ret.finished_time = int(rd.dict_find_int_value("finished_time"));
		ret.seeding_time = int(rd.dict_find_int_value("seeding_time"));

		ret.last_seen_complete = rd.dict_find_int_value("last_seen_complete");

		// scrape data cache
		ret.num_complete = int(rd.dict_find_int_value("num_complete", -1));
		ret.num_incomplete = int(rd.dict_find_int_value("num_incomplete", -1));
		ret.num_downloaded = int(rd.dict_find_int_value("num_downloaded", -1));

		// torrent settings
		ret.max_uploads = int(rd.dict_find_int_value("max_uploads", -1));
		ret.max_connections = int(rd.dict_find_int_value("max_connections", -1));
		ret.upload_limit = int(rd.dict_find_int_value("upload_rate_limit", -1));
		ret.download_limit = int(rd.dict_find_int_value("download_rate_limit", -1));

		// torrent state
		apply_flag(ret.flags, rd, "seed_mode", add_torrent_params::flag_seed_mode);
		apply_flag(ret.flags, rd, "super_seeding", add_torrent_params::flag_super_seeding);
		apply_flag(ret.flags, rd, "auto_managed", add_torrent_params::flag_auto_managed);
		apply_flag(ret.flags, rd, "sequential_download", add_torrent_params::flag_sequential_download);
		apply_flag(ret.flags, rd, "paused", add_torrent_params::flag_paused);

		ret.save_path = rd.dict_find_string_value("save_path").to_string();

		ret.url = rd.dict_find_string_value("url").to_string();
#ifndef TORRENT_NO_DEPRECATE
		// deprecated in 1.2
		ret.uuid = rd.dict_find_string_value("uuid").to_string();
#endif

		bdecode_node mapped_files = rd.dict_find_list("mapped_files");
		if (mapped_files)
		{
			for (int i = 0; i < mapped_files.list_size(); ++i)
			{
				auto new_filename = mapped_files.list_string_value_at(i);
				if (new_filename.empty()) continue;
				ret.renamed_files[i] = new_filename.to_string();
			}
		}

		ret.added_time = rd.dict_find_int_value("added_time", 0);
		ret.completed_time = rd.dict_find_int_value("completed_time", 0);

		// load file priorities except if the add_torrent_param file was set to
		// override resume data
		bdecode_node file_priority = rd.dict_find_list("file_priority");
		if (file_priority)
		{
			int const num_files = file_priority.list_size();
			ret.file_priorities.resize(num_files, 4);
			for (int i = 0; i < num_files; ++i)
			{
				ret.file_priorities[i] = std::uint8_t(file_priority.list_int_value_at(i, 1));
				// this is suspicious, leave seed mode
				if (ret.file_priorities[i] == 0)
				{
					ret.flags &= ~add_torrent_params::flag_seed_mode;
				}
			}
		}

		bdecode_node trackers = rd.dict_find_list("trackers");
		if (trackers)
		{
			// it's possible to delete the trackers from a torrent and then save
			// resume data with an empty trackers list. Since we found a trackers
			// list here, these should replace whatever we find in the .torrent
			// file.
			ret.flags |= add_torrent_params::flag_override_trackers;

			int tier = 0;
			for (int i = 0; i < trackers.list_size(); ++i)
			{
				bdecode_node tier_list = trackers.list_at(i);
				if (!tier_list || tier_list.type() != bdecode_node::list_t)
					continue;

				for (int j = 0; j < tier_list.list_size(); ++j)
				{
					ret.trackers.push_back(tier_list.list_string_value_at(j).to_string());
					ret.tracker_tiers.push_back(tier);
				}
				++tier;
			}
		}

		// if merge resume http seeds is not set, we need to clear whatever web
		// seeds we loaded from the .torrent file, because we want whatever's in
		// the resume file to take precedence. If there aren't even any fields in
		// the resume data though, keep the ones from the torrent
		bdecode_node url_list = rd.dict_find_list("url-list");
		bdecode_node httpseeds = rd.dict_find_list("httpseeds");
		if (url_list || httpseeds)
		{
			// since we found http seeds in the resume data, they should replace
			// whatever web seeds are specified in the .torrent, by default
			ret.flags |= add_torrent_params::flag_override_web_seeds;
		}

		if (url_list)
		{
			for (int i = 0; i < url_list.list_size(); ++i)
			{
				auto url = url_list.list_string_value_at(i);
				if (url.empty()) continue;
				ret.url_seeds.push_back(url.to_string());
			}
		}

		if (httpseeds)
		{
			for (int i = 0; i < httpseeds.list_size(); ++i)
			{
				auto url = httpseeds.list_string_value_at(i);
				if (url.empty()) continue;
				ret.http_seeds.push_back(url.to_string());
			}
		}

		bdecode_node mt = rd.dict_find_string("merkle tree");
		if (mt)
		{
			ret.merkle_tree.resize(mt.string_length() / 20);
			std::memcpy(&ret.merkle_tree[0], mt.string_ptr()
				, int(ret.merkle_tree.size()) * 20);
		}

		// some sanity checking. Maybe we shouldn't be in seed mode anymore
		if (bdecode_node pieces = rd.dict_find_string("pieces"))
		{
			char const* pieces_str = pieces.string_ptr();
			int const pieces_len = pieces.string_length();
			ret.have_pieces.resize(pieces_len);
			ret.verified_pieces.resize(pieces_len);
			for (int i = 0; i < pieces_len; ++i)
			{
				// being in seed mode and missing a piece is not compatible.
				// Leave seed mode if that happens
				if (pieces_str[i] & 1) ret.have_pieces.set_bit(i);
				else ret.have_pieces.clear_bit(i);

				if (pieces_str[i] & 2) ret.verified_pieces.set_bit(i);
				else ret.verified_pieces.clear_bit(i);
			}
		}

		if (bdecode_node piece_priority = rd.dict_find_string("piece_priority"))
		{
			char const* prio_str = piece_priority.string_ptr();
			ret.piece_priorities.resize(piece_priority.string_length());
			for (int i = 0; i < piece_priority.string_length(); ++i)
			{
				ret.piece_priorities[i] = prio_str[i];
			}
		}

		using namespace libtorrent::detail; // for read_*_endpoint()
		if (bdecode_node peers_entry = rd.dict_find_string("peers"))
		{
			char const* ptr = peers_entry.string_ptr();
			for (int i = 0; i < peers_entry.string_length(); i += 6)
				ret.peers.push_back(read_v4_endpoint<tcp::endpoint>(ptr));
		}

#if TORRENT_USE_IPV6
		if (bdecode_node peers_entry = rd.dict_find_string("peers6"))
		{
			char const* ptr = peers_entry.string_ptr();
			for (int i = 0; i < peers_entry.string_length(); i += 18)
				ret.peers.push_back(read_v6_endpoint<tcp::endpoint>(ptr));
		}
#endif

		if (bdecode_node peers_entry = rd.dict_find_string("banned_peers"))
		{
			char const* ptr = peers_entry.string_ptr();
			for (int i = 0; i < peers_entry.string_length(); i += 6)
				ret.banned_peers.push_back(read_v4_endpoint<tcp::endpoint>(ptr));
		}

#if TORRENT_USE_IPV6
		if (bdecode_node peers_entry = rd.dict_find_string("banned_peers6"))
		{
			char const* ptr = peers_entry.string_ptr();
			for (int i = 0; i < peers_entry.string_length(); i += 18)
				ret.banned_peers.push_back(read_v6_endpoint<tcp::endpoint>(ptr));
		}
#endif

		// parse unfinished pieces
		if (bdecode_node unfinished_entry = rd.dict_find_list("unfinished"))
		{
			for (int i = 0; i < unfinished_entry.list_size(); ++i)
			{
				bdecode_node e = unfinished_entry.list_at(i);
				if (e.type() != bdecode_node::dict_t) continue;
				int piece = int(e.dict_find_int_value("piece", -1));
				if (piece < 0) continue;

				bdecode_node bitmask = e.dict_find_string("bitmask");
				if (bitmask || bitmask.string_length() == 0) continue;
				bitfield& bf = ret.unfinished_pieces[piece];
				bf.assign(bitmask.string_ptr(), bitmask.string_length());
			}
		}

		ret.flags &= ~add_torrent_params::flag_need_save_resume;

		return ret;
	}

	add_torrent_params read_resume_data(char const* buffer, int size, error_code& ec)
	{
		bdecode_node rd;
		bdecode(buffer, buffer + size, rd, ec);
		if (ec) return add_torrent_params();

		return read_resume_data(rd, ec);
	}
}
