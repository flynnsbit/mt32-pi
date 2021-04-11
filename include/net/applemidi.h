//
// applemidi.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2021 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _applemidi_h
#define _applemidi_h

#include <circle/bcmrandom.h>
#include <circle/net/ipaddress.h>
#include <circle/net/socket.h>
#include <circle/sched/task.h>

class CAppleMIDIParticipant : protected CTask
{
public:
	using TMIDIReceiveHandler = void (*)(const u8* pData, size_t nSize);

	CAppleMIDIParticipant(CBcmRandomNumberGenerator* pRandom);
	~CAppleMIDIParticipant();

	bool Initialize();
	void RegisterMIDIReceiveHandler(TMIDIReceiveHandler pHandler) { m_pReceiveHandler = pHandler; }

	virtual void Run() override;

private:
	void ControlInvitationState();
	void MIDIInvitationState();
	void ConnectedState();
	void Reset();

	bool SendPacket(CSocket* pSocket, u16 nPort, const void* pData, size_t nSize);
	bool SendAcceptInvitationPacket(CSocket* pSocket, u16 nPort);
	bool SendSyncPacket(u64 nTimestamp1, u64 nTimestamp2);
	bool SendFeedbackPacket();

	CBcmRandomNumberGenerator* m_pRandom;

	// UDP sockets
	CSocket* m_pControlSocket;
	CSocket* m_pMIDISocket;

	// Socket receive buffers
	CIPAddress m_InitiatorIPAddress;
	u16 m_nInitiatorControlPort;
	u16 m_nInitiatorMIDIPort;
	u8 m_ControlBuffer[FRAME_BUFFER_SIZE];
	u8 m_MIDIBuffer[FRAME_BUFFER_SIZE];

	int m_nControlResult;
	int m_nMIDIResult;

	// Handler for received MIDI packets
	TMIDIReceiveHandler m_pReceiveHandler;

	// Participant state machine
	enum class TState
	{
		ControlInvitation,
		MIDIInvitation,
		Connected
	};

	TState m_State;

	u32 m_nInitiatorToken = 0;
	u32 m_nInitiatorSSRC = 0;
	u32 m_nSSRC = 0;
	u32 m_nLastMIDISequenceNumber = 0;

	u64 m_nOffsetEstimate = 0;
	u64 m_nLastSyncTime = 0;

	u16 m_nSequence = 0;
	u16 m_nLastFeedbackSequence = 0;
	u64 m_nLastFeedbackTime = 0;
};

#endif
