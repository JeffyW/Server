#include "mob_movement_manager.h"
#include "client.h"
#include "mob.h"
#include "zone.h"
#include "position.h"
#include "water_map.h"
#include "../common/eq_packet_structs.h"
#include "../common/misc_functions.h"
#include "../common/data_verification.h"

#include <vector>
#include <deque>
#include <map>
#include <stdlib.h>

extern double frame_time;
extern Zone *zone;

class IMovementCommand
{
public:
	IMovementCommand() = default;
	virtual ~IMovementCommand() = default;
	virtual bool Process(MobMovementManager *mgr, Mob *m) = 0;
	virtual bool Started() const = 0;
};

class RotateToCommand : public IMovementCommand
{
public:
	RotateToCommand(double rotate_to, double dir, MobMovementMode mode) {
		m_rotate_to = rotate_to;
		m_rotate_to_dir = dir;
		m_rotate_to_mode = mode;
		m_started = false;
	}

	virtual ~RotateToCommand() {

	}

	virtual bool Process(MobMovementManager *mgr, Mob *m) {
		if (!m->IsAIControlled()) {
			return true;
		}

		auto rotate_to_speed = m_rotate_to_mode == MovementRunning ? 50.0 :  16.0; //todo: get this from mob

		if (!m_started) {
			m_started = true;
			m->SetMoving(true);
		
			if (rotate_to_speed > 0.0 && rotate_to_speed <= 25.0) { //send basic rotation
				mgr->SendCommandToClients(m, 0.0, 0.0, 0.0, m_rotate_to_dir * rotate_to_speed, 0, ClientRangeClose);
			}
		}
		
		auto from = FixHeading(m->GetHeading());
		auto to = FixHeading(m_rotate_to);
		auto diff = to - from;
		
		while (diff < -256.0) {
			diff += 512.0;
		}
		
		while (diff > 256) {
			diff -= 512.0;
		}
		
		auto dist = std::abs(diff);
		auto td = rotate_to_speed * 19.0 * frame_time;
		
		if (td >= dist) {
			m->SetHeading(to);
			m->SetMoving(false);
			mgr->SendCommandToClients(m, 0.0, 0.0, 0.0, 0.0, 0, ClientRangeCloseMedium);
			return true;
		}
		
		from += td * m_rotate_to_dir;
		m->SetHeading(FixHeading(from));
		return false;
	}

	virtual bool Started() const {
		return m_started;
	}

private:
	double m_rotate_to;
	double m_rotate_to_dir;
	MobMovementMode m_rotate_to_mode;
	bool m_started;
};

class MoveToCommand : public IMovementCommand
{
public:
	MoveToCommand(float x, float y, float z, MobMovementMode mode) {
		m_move_to_x = x;
		m_move_to_y = y;
		m_move_to_z = z;
		m_move_to_mode = mode;
		m_last_sent_time = 0.0;
		m_last_sent_speed = 0;
		m_started = false;
		m_total_h_dist = 0.0;
		m_total_v_dist = 0.0;
	}

	virtual ~MoveToCommand() {

	}

	virtual bool Process(MobMovementManager *mgr, Mob *m) {
		if (!m->IsAIControlled()) {
			return true;
		}

		//Send a movement packet when you start moving		
		double current_time = static_cast<double>(Timer::GetCurrentTime()) / 1000.0;
		int current_speed = 0;

		if (m_move_to_mode == MovementRunning) {
			current_speed = m->GetRunspeed();
		}
		else {
			current_speed = m->GetWalkspeed();
		}

		if (!m_started) {
			m_started = true;
			//rotate to the point
			m->SetMoving(true);
			m->SetHeading(m->CalculateHeadingToTarget(m_move_to_x, m_move_to_y));
			m->TryFixZ();

			m_last_sent_speed = current_speed;
			m_last_sent_time = current_time;
			m_total_h_dist = DistanceNoZ(m->GetPosition(), glm::vec4(m_move_to_x, m_move_to_z, 0.0f, 0.0f));
			m_total_v_dist = m_move_to_z - m->GetZ();
			mgr->SendCommandToClients(m, 0.0, 0.0, 0.0, 0.0, current_speed, ClientRangeCloseMedium);
			return false;
		}

		//When speed changes
		if (current_speed != m_last_sent_speed) {
			m->TryFixZ();

			m_last_sent_speed = current_speed;
			m_last_sent_time = current_time;
			mgr->SendCommandToClients(m, 0.0, 0.0, 0.0, 0.0, current_speed, ClientRangeCloseMedium);
			return false;
		}

		//If x seconds have passed without sending an update.
		if (current_time - m_last_sent_time >= 5.0) {
			m->TryFixZ();

			m_last_sent_speed = current_speed;
			m_last_sent_time = current_time;
			mgr->SendCommandToClients(m, 0.0, 0.0, 0.0, 0.0, current_speed, ClientRangeCloseMedium);
			return false;
		}

		auto &p = m->GetPosition();
		glm::vec2 tar(m_move_to_x, m_move_to_y);
		glm::vec2 pos(p.x, p.y);
		double len = glm::distance(pos, tar);
		if (len == 0) {
			m->SetMoving(false);
			return true;
		}

		m->SetMoved(true);

		glm::vec2 dir = tar - pos;
		glm::vec2 ndir = glm::normalize(dir);
		double distance_moved = frame_time * current_speed * 0.4f * 1.45f;

		if (distance_moved > len) {
			m->SetPosition(m_move_to_x, m_move_to_y, m_move_to_z);
		
			if (m->IsNPC()) {
				entity_list.ProcessMove(m->CastToNPC(), m_move_to_x, m_move_to_y, m_move_to_z);
			}
		
			m->TryFixZ();
			m->SetMoving(false);
			return true;
		}
		else {
			glm::vec2 npos = pos + (ndir * static_cast<float>(distance_moved));
			
			len -= distance_moved;
			double total_distance_traveled = m_total_h_dist - len;
			double start_z = m_move_to_z - m_total_v_dist;
			double z_at_pos = start_z + (m_total_v_dist * (total_distance_traveled / m_total_h_dist));

			m->SetPosition(npos.x, npos.y, z_at_pos);
		
			if (m->IsNPC()) {
				entity_list.ProcessMove(m->CastToNPC(), npos.x, npos.y, z_at_pos);
			}
		}
		
		return false;
	}

	virtual bool Started() const {
		return m_started;
	}

private:

	double m_move_to_x;
	double m_move_to_y;
	double m_move_to_z;
	MobMovementMode m_move_to_mode;
	bool m_started;

	double m_last_sent_time;
	int m_last_sent_speed;
	double m_total_h_dist;
	double m_total_v_dist;
};

class TeleportToCommand : public IMovementCommand
{
public:
	TeleportToCommand(float x, float y, float z, float heading) {
		m_teleport_to_x = x;
		m_teleport_to_y = y;
		m_teleport_to_z = z;
		m_teleport_to_heading = heading;
	}

	virtual ~TeleportToCommand() {

	}

	virtual bool Process(MobMovementManager *mgr, Mob *m) {
		if (!m->IsAIControlled()) {
			return true;
		}

		m->SetPosition(m_teleport_to_x, m_teleport_to_y, m_teleport_to_z);
		m->SetHeading(mgr->FixHeading(m_teleport_to_heading));
		mgr->SendCommandToClients(m, 0.0, 0.0, 0.0, 0.0, 0, ClientRangeAny);

		if (m->IsNPC()) {
			entity_list.ProcessMove(m->CastToNPC(), m_teleport_to_x, m_teleport_to_y, m_teleport_to_z);
		}

		return true;
	}

	virtual bool Started() const {
		return false;
	}

private:

	double m_teleport_to_x;
	double m_teleport_to_y;
	double m_teleport_to_z;
	double m_teleport_to_heading;
};

class StopMovingCommand : public IMovementCommand
{
public:
	StopMovingCommand() {
	}

	virtual ~StopMovingCommand() {

	}

	virtual bool Process(MobMovementManager *mgr, Mob *m) {
		if (!m->IsAIControlled()) {
			return true;
		}

		if (m->IsMoving()) {
			m->SetMoving(false);
			mgr->SendCommandToClients(m, 0.0, 0.0, 0.0, 0.0, 0, ClientRangeCloseMedium);
		}
		return true;
	}

	virtual bool Started() const {
		return false;
	}
};

struct MovementStats
{
	MovementStats() {
		LastResetTime = static_cast<double>(Timer::GetCurrentTime()) / 1000.0;
		TotalSent = 0ULL;
		TotalSentMovement = 0ULL;
		TotalSentPosition = 0ULL;
		TotalSentHeading = 0ULL;
	}

	double LastResetTime;
	uint64_t TotalSent;
	uint64_t TotalSentMovement;
	uint64_t TotalSentPosition;
	uint64_t TotalSentHeading;
};

struct NavigateTo
{
	NavigateTo() {
		navigate_to_x = 0.0;
		navigate_to_y = 0.0;
		navigate_to_z = 0.0;
		navigate_to_heading = 0.0;
		last_set_time = 0.0;
	}

	double navigate_to_x;
	double navigate_to_y;
	double navigate_to_z;
	double navigate_to_heading;
	double last_set_time;
};

struct MobMovementEntry
{
	std::deque<std::unique_ptr<IMovementCommand>> Commands;
	NavigateTo NavTo;
};

void AdjustRoute(std::list<IPathfinder::IPathNode> &nodes, int flymode, float offset) {
	if (!zone->HasMap() || !zone->HasWaterMap()) {
		return;
	}

	for (auto &node : nodes) {
		if (flymode == GravityBehavior::Ground || !zone->watermap->InLiquid(node.pos)) {
			auto best_z = zone->zonemap->FindBestZ(node.pos, nullptr);
			if (best_z != BEST_Z_INVALID) {
				node.pos.z = best_z + offset;
			}
		}
	}
}

struct MobMovementManager::Implementation
{
	std::map<Mob*, MobMovementEntry> Entries;
	std::vector<Client*> Clients;
	MovementStats Stats;
};

MobMovementManager::MobMovementManager()
{
	_impl.reset(new Implementation());
}

MobMovementManager::~MobMovementManager()
{
}

void MobMovementManager::Process()
{
	for (auto &iter : _impl->Entries) {
		auto &ent = iter.second;
		auto &commands = ent.Commands;

		while (true != commands.empty()) {
			auto &cmd = commands.front();
			auto r = cmd->Process(this, iter.first);

			if (true != r) {
				break;
			}

			commands.pop_front();
		}
	}
}

void MobMovementManager::AddMob(Mob *m)
{
	_impl->Entries.insert(std::make_pair(m, MobMovementEntry()));
}

void MobMovementManager::RemoveMob(Mob *m)
{
	_impl->Entries.erase(m);
}

void MobMovementManager::AddClient(Client *c)
{
	_impl->Clients.push_back(c);
}

void MobMovementManager::RemoveClient(Client *c)
{
	auto iter = _impl->Clients.begin();
	while (iter != _impl->Clients.end()) {
		if (c == *iter) {
			_impl->Clients.erase(iter);
			return;
		}
	
		++iter;
	}
}

void MobMovementManager::RotateTo(Mob *who, float to, MobMovementMode mode)
{
	auto iter = _impl->Entries.find(who);
	auto &ent = (*iter);

	if (true != ent.second.Commands.empty()) {
		return;
	}
	
	PushRotateTo(ent.second, who, to, mode);
}

void MobMovementManager::Teleport(Mob *who, float x, float y, float z, float heading)
{
	auto iter = _impl->Entries.find(who);
	auto &ent = (*iter);
	
	ent.second.Commands.clear();

	PushTeleportTo(ent.second, x, y, z, heading);
}

void MobMovementManager::NavigateTo(Mob *who, float x, float y, float z, MobMovementMode mode)
{
	auto iter = _impl->Entries.find(who);
	auto &ent = (*iter);
	auto &nav = ent.second.NavTo;

	double current_time = static_cast<double>(Timer::GetCurrentTime()) / 1000.0;
	if ((current_time - nav.last_set_time) > 0.5) {
		//Can potentially recalc
	
		auto within = IsPositionWithinSimpleCylinder(glm::vec3(x, y, z), glm::vec3(nav.navigate_to_x, nav.navigate_to_y, nav.navigate_to_z), 1.5f, 6.0f);
		auto heading_match = IsHeadingEqual(0.0, nav.navigate_to_heading);

		if (false == within || false == heading_match) {
			ent.second.Commands.clear();

			//Path is no longer valid, calculate a new path
			UpdatePath(who, x, y, z, mode);
			nav.navigate_to_x = x;
			nav.navigate_to_y = y;
			nav.navigate_to_z = z;
			nav.navigate_to_heading = 0.0;
			nav.last_set_time = current_time;
		}
	}
}

void MobMovementManager::StopNavigation(Mob *who) {
	auto iter = _impl->Entries.find(who);
	auto &ent = (*iter);
	
	if (true == ent.second.Commands.empty()) {
		return;
	}

	auto &running_cmd = ent.second.Commands.front();
	if (false == running_cmd->Started()) {
		ent.second.Commands.clear();
		return;
	}

	if (who->IsMoving()) {
		who->TryFixZ();
		who->SetMoving(false);
		SendCommandToClients(who, 0.0, 0.0, 0.0, 0.0, 0, ClientRangeCloseMedium);
	}
	ent.second.Commands.clear();
}

void MobMovementManager::SendCommandToClients(Mob *m, float dx, float dy, float dz, float dh, int anim, ClientRange range)
{
	if (range == ClientRangeNone) {
		return;
	}

	EQApplicationPacket outapp(OP_ClientUpdate, sizeof(PlayerPositionUpdateServer_Struct));
	PlayerPositionUpdateServer_Struct *spu = (PlayerPositionUpdateServer_Struct*)outapp.pBuffer;
	FillCommandStruct(spu, m, dx, dy, dz, dh, anim);

	if (range == ClientRangeAny) {
		for (auto &c : _impl->Clients) {
			_impl->Stats.TotalSent++;

			if (anim != 0) {
				_impl->Stats.TotalSentMovement++;
			}
			else if (dh != 0) {
				_impl->Stats.TotalSentHeading++;
			}
			else {
				_impl->Stats.TotalSentPosition++;
			}

			c->QueuePacket(&outapp, false);
		}
	}
	else {
		for (auto &c : _impl->Clients) {
			float dist = c->CalculateDistance(m->GetX(), m->GetY(), m->GetZ());

			bool match = false;
			if (range & ClientRangeClose) {
				if (dist < 250.0f) {
					match = true;
				}
			}

			if (!match && range & ClientRangeMedium) {
				if (dist >= 250.0f && dist < 1500.0f) {
					match = true;
				}
			}

			if (!match && range & ClientRangeLong) {
				if (dist >= 1500.0f) {
					match = true;
				}
			}

			if (match) {
				_impl->Stats.TotalSent++;

				if (anim != 0) {
					_impl->Stats.TotalSentMovement++;
				}
				else if (dh != 0) {
					_impl->Stats.TotalSentHeading++;
				}
				else {
					_impl->Stats.TotalSentPosition++;
				}

				c->QueuePacket(&outapp, false);
			}
		}
	}
}

float MobMovementManager::FixHeading(float in)
{
	auto h = in;
	while (h > 512.0) {
		h -= 512.0;
	}

	while (h < 0.0) {
		h += 512.0;
	}

	return h;
}

void MobMovementManager::DumpStats(Client *to)
{
	auto current_time = static_cast<double>(Timer::GetCurrentTime()) / 1000.0;
	auto total_time = current_time - _impl->Stats.LastResetTime;

	to->Message(MT_System, "Dumping Movement Stats:");
	to->Message(MT_System, "Total Sent: %u (%.2f / sec)", _impl->Stats.TotalSent, static_cast<double>(_impl->Stats.TotalSent) / total_time);
	to->Message(MT_System, "Total Heading: %u (%.2f / sec)", _impl->Stats.TotalSentHeading, static_cast<double>(_impl->Stats.TotalSentHeading) / total_time);
	to->Message(MT_System, "Total Movement: %u (%.2f / sec)", _impl->Stats.TotalSentMovement, static_cast<double>(_impl->Stats.TotalSentMovement) / total_time);
	to->Message(MT_System, "Total Position: %u (%.2f / sec)", _impl->Stats.TotalSentPosition, static_cast<double>(_impl->Stats.TotalSentPosition) / total_time);
}

void MobMovementManager::ClearStats()
{
	_impl->Stats.LastResetTime = static_cast<double>(Timer::GetCurrentTime()) / 1000.0;
	_impl->Stats.TotalSent = 0;
	_impl->Stats.TotalSentHeading = 0;
	_impl->Stats.TotalSentMovement = 0;
	_impl->Stats.TotalSentPosition = 0;
}

void MobMovementManager::FillCommandStruct(PlayerPositionUpdateServer_Struct *spu, Mob *m, float dx, float dy, float dz, float dh, int anim)
{
	memset(spu, 0x00, sizeof(PlayerPositionUpdateServer_Struct));
	spu->spawn_id = m->GetID();
	spu->x_pos = FloatToEQ19(m->GetX());
	spu->y_pos = FloatToEQ19(m->GetY());
	spu->z_pos = FloatToEQ19(m->GetZ());
	spu->heading = FloatToEQ12(m->GetHeading());
	spu->delta_x = FloatToEQ13(dx);
	spu->delta_y = FloatToEQ13(dy);
	spu->delta_z = FloatToEQ13(dz);
	spu->delta_heading = FloatToEQ10(dh);
	spu->animation = anim;
}

void MobMovementManager::UpdatePath(Mob *who, float x, float y, float z, MobMovementMode mode)
{
	bool partial = false;
	bool stuck = false;
	auto route = zone->pathing->FindRoute(glm::vec3(who->GetX(), who->GetY(), who->GetZ()), glm::vec3(x, y, z), partial, stuck);

	if (route.empty()) {
		return;
	}

	auto &first = route.front();

	//if who is already at the first node, then cull it
	if (IsPositionEqualWithinCertainZ(glm::vec3(who->GetX(), who->GetY(), who->GetZ()), first.pos, 5.0f)) {
		route.pop_front();
	}

	if (route.empty()) {
		return;
	}

	AdjustRoute(route, who->GetFlyMode(), who->GetZOffset());

	auto iter = _impl->Entries.find(who);
	auto &ent = (*iter);

	first = route.front();
	//If mode = walking then rotateto first node (if possible, live does this)
	if (mode == MovementWalking) {
		auto h = who->CalculateHeadingToTarget(first.pos.x, first.pos.y);
		PushRotateTo(ent.second, who, h, mode);
	}

	//for each node create a moveto/teleport command
	glm::vec3 previous_pos(who->GetX(), who->GetY(), who->GetZ());
	for (auto &node : route) {
		if (node.teleport) {
			PushTeleportTo(ent.second, node.pos.x, node.pos.y, node.pos.z, 
				CalculateHeadingAngleBetweenPositions(previous_pos.x, previous_pos.y, node.pos.x, node.pos.y));
		}
		else {
			PushMoveTo(ent.second, node.pos.x, node.pos.y, node.pos.z, mode);
		}

		previous_pos = node.pos;
	}

	if (stuck) {
		PushTeleportTo(ent.second, x, y, z, 
			CalculateHeadingAngleBetweenPositions(previous_pos.x, previous_pos.y, x, y));
	}
	else {
		PushStopMoving(ent.second);
	}
}

void MobMovementManager::PushTeleportTo(MobMovementEntry &ent, float x, float y, float z, float heading)
{
	ent.Commands.push_back(std::unique_ptr<IMovementCommand>(new TeleportToCommand(x, y, z, heading)));
}

void MobMovementManager::PushMoveTo(MobMovementEntry &ent, float x, float y, float z, MobMovementMode mode)
{
	ent.Commands.push_back(std::unique_ptr<IMovementCommand>(new MoveToCommand(x, y, z, mode)));
}

void MobMovementManager::PushRotateTo(MobMovementEntry &ent, Mob *who, float to, MobMovementMode mode)
{
	auto from = FixHeading(who->GetHeading());
	to = FixHeading(to);

	float diff = to - from;

	if (std::abs(diff) < 0.001f) {
		return;
	}

	while (diff < -256.0) {
		diff += 512.0;
	}

	while (diff > 256) {
		diff -= 512.0;
	}

	ent.Commands.push_back(std::unique_ptr<IMovementCommand>(new RotateToCommand(to, diff > 0 ? 1.0 : -1.0, mode)));
}

void MobMovementManager::PushStopMoving(MobMovementEntry &ent)
{
	ent.Commands.push_back(std::unique_ptr<IMovementCommand>(new StopMovingCommand()));
}