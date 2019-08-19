// Copyright (C) 2019 Bluzelle
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License, version 3,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include <peers_beacon/peers_beacon.hpp>

using namespace bzn;

peers_beacon::peers_beacon(std::shared_ptr<bzn::options_base> opt)
        : options(opt)
{}

void
peers_beacon::start()
{
    if (!this->refresh(true))
    {
        throw std::runtime_error("could not construct initial peers list)");
    }

    this->run_timer();
}

void
peers_beacon::run_timer()
{
    this->refresh_timer->cancel();
    this->refresh_timer->expires_from_now(this->options->get_simple_options().get<uint64_t>(std::chrono::seconds{bzn::option_names::PEERS_REFRESH_INTERVAL_SECONDS}));
    this->refresh_timer->async_wait([weak_self = weak_from_this()]()
                                    {
                                        auto self = weak_self.lock();
                                        if (self)
                                        {
                                            self->refresh();
                                            self->run_timer();
                                        }
                                    });
}

const std::shared_ptr<const peers_list_t>
peers_beacon::current() const
{
    return this->internal_current;
}

bool
peers_beacon::refresh(bool first_run)
{
    bool has_esr = !this->options->get_swarm_id().empty()
            && !this->options->get_swarm_info_esr_address().empty()
            && !this->options->get_swarm_info_esr_url().empty();
    bool has_file = !this->options->get_bootstrap_peers_file().empty();
    bool has_url = !this->options->get_bootstrap_peers_file().empty();

    /* Here we chose not to fall back on another method if the first priority from our config is unavailable. This is
     * chosen so that if, eg, a url based peers list is briefly unavailable, we do not abruptly switch to a very old
     * peers list from an unmaintained esr contract.
     */

    if (has_esr)
    {
        if (first_run)
        {
            LOG(info) << "ESR chosen as peers source";
        }
        return this->fetch_from_esr();
    }

    if (has_url)
    {
        if (first_run)
        {
            LOG(info) << "URL chosen as peers source";
        }
        return this->fetch_from_url();
    }

    if (has_file)
    {
        if (first_run)
        {
            LOG(info) << "file chosen as peers source";
        }
        return this->fetch_from_file();
    }

    throw std::runtime_error("no acceptable source for peers configured; must specify file or url or esr");
}

bool
peers_beacon::fetch_from_file()
{
    std::ifstream file(this->options->get_bootstrap_peers_file());
    if (file.fail())
    {
        LOG(error) << "Failed to read bootstrap peers file " << filename;
        return false;
    }

    LOG(info) << "Reading peers from " << filename;

    return parse_and_save_peers(file);
}

bool
peers_beacon::fetch_from_url()
{
    std::string peers = bzn::utils::http::sync_req(this->options->get_boostrap_peers_url());

    LOG(info) << "Downloaded peer list from " << url;

    std::stringstream stream;
    stream << peers;

    return parse_and_save_peers(stream);
}

bool
peers_beacon::fetch_from_esr()
{
    auto swarm_id = this->options->get_swarm_id();
    auto esr_address = this->options->get_swarm_info_esr_address();
    auto esr_url = this->options->get_swarm_info_esr_url();

    peer_list_t new_peers;
    auto peer_ids = bzn::utils::esr::get_peer_ids(swarm_id, esr_address, esr_url);
    for (const auto& peer_id : peer_ids)
    {
        bzn::peer_address_t peer_info{bzn::utils::esr::get_peer_info(swarm_id, peer_id, esr_address, esr_url)};
        if (peer_info.host.empty()
            || peer_info.port == 0
            //|| peer_info.name.empty() // is it important that a peer have a name?
            || peer_info.uuid.empty()
                )
        {
            LOG(warning) << "Invalid peer information found in esr contract, ignoring info for peer: " << peer_id << " in swarm: " << swarm_id;
        }
        else
        {
            new_peers.emplace(peer_info);
        }
    }

    return this->switch_peers_list(new_peers);
}

bool
peers_beacon::switch_peers_list(const peers_list_t& new_peers)
{
    if (new_peers.size() == 0)
    {
        LOG(error) << "Failed to read any peers";
        if (this->internal_current->size() > 0)
        {
            LOG(error) << "Keeping old peer list";
        }
        else
        {
            LOG(error) << "Old peers list also empty";
            throw std::runtime_error("failed to find a valid peers list");
        }
        return false;
    }

    if(*(this->internal_current) != new_peers)
    {
        LOG(info) << "Switching to new peers list with " << new_peers.size() << " peers";
    }

    this->internal_current = std::make_shared<const peers_list_t>(new_peers);

    return true;
}

bool
peers_beacon::parse_and_save_peers(std::istream& source)
{
    Json::Value root;

    try
    {
        peers >> root;
    }
    catch (const std::exception& e)
    {
        LOG(error) << "Failed to parse peer JSON (" << e.what() << ")";
        LOG(error) << "Keeping old peer list";
        return false;
    }

    auto new_peers = this->build_peers_list_from_json(root);
    return this->switch_peers_list(new_peers);
}

peers_list_t
peers_beacon::build_peers_list_from_json(const json::Value& root)
{
    peers_list_t result;

    // Expect the read json to be an array of peer objects
    for (const auto& peer : root)
    {
        std::string host;
        std::string name;
        std::string uuid;
        uint16_t    port;

        try
        {
            host = peer["host"].asString();
            port = peer["port"].asUInt();
            uuid = peer.isMember("uuid") ? peer["uuid"].asString() : "unknown";
            name = peer.isMember("name") ? peer["name"].asString() : "unknown";
        }
        catch(std::exception& e)
        {
            LOG(warning) << "Ignoring malformed peer specification " << peer;
            continue;
        }

        // port wasn't actually a 16 bit uint
        if (peer["port"].asUInt() != port)
        {
            LOG(warning) << "Ignoring peer with bad port " << peer;
            continue;
        }

        // peer didn't contain everything we need
        if (host.empty() || port == 0)
        {
            LOG(warning) << "Ignoring underspecified peer (needs host and port) " << peer;
            continue;
        }

        LOG(trace) << "Found peer " << host << ":" << port << " (" << name << ")";

        result.emplace(host, port, name, uuid);
    }

    LOG(trace) << "Found " << result.size() << " well formed peers";
    return result;
}