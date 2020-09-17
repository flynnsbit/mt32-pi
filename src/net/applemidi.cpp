//
// applemidi.cpp
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

#include <circle/logger.h>
#include <circle/net/in.h>
#include <circle/net/netsubsystem.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/util.h>

#include "net/applemidi.h"
#include "net/byteorder.h"

// #define APPLEMIDI_DEBUG

constexpr u16 ControlPort = 5000;
constexpr u16 MIDIPort    = ControlPort + 1;

constexpr u16 AppleMIDISignature = 0xFFFF;
constexpr u8 AppleMIDIVersion    = 2;

constexpr u8 RTPMIDIPayloadType = 0x61;
constexpr u8 RTPMIDIVersion     = 2;

// Timeout period for sync packets (60 seconds in 100 microsecond units)
constexpr unsigned int SyncTimeout = 60 * 10000;

// Receiver feedback packet frequency (1 second in 100 microsecond units)
constexpr unsigned int ReceiverFeedbackPeriod = 1 * 10000;

const char AppleMIDIName[] = "applemidi";

constexpr u16 CommandWord(const char Command[2]) { return Command[0] << 8 | Command[1]; }

enum TAppleMIDICommand : u16
{
	Invitation         = CommandWord("IN"),
	InvitationAccepted = CommandWord("OK"),
	Sync               = CommandWord("CK"),
	ReceiverFeedback   = CommandWord("RS"),
	EndSession         = CommandWord("BY"),
};

#pragma pack(push, 1)
struct TAppleMIDIInvitation
{
	u16 nSignature;
	u16 nCommand;
	u32 nVersion;
	u32 nInitiatorToken;
	u32 nSSRC;
	char Name[256];
};

struct TAppleMIDIEndSession
{
	u16 nSignature;
	u16 nCommand;
	u32 nVersion;
	u32 nInitiatorToken;
	u32 nSSRC;
};

struct TAppleMIDISync
{
	u16 nSignature;
	u16 nCommand;
	u32 nSSRC;
	u8 nCount;
	u8 nPadding[3];
	u64 nTimestamps[3];
};

struct TAppleMIDIReceiverFeedback
{
	u16 nSignature;
	u16 nCommand;
	u32 nSSRC;
	u32 nSequence;
};

struct TRTPHeader
{
	u16 nFlags;
	u16 nSequence;
	u32 nTimestamp;
	u32 nSSRC;
};

struct TRTPMIDI
{
	TRTPHeader RTPHeader;
	u8 nHeader;
};
#pragma pack(pop)

u64 GetSyncClock()
{
	static const u64 nStartTime = CTimer::GetClockTicks();
	const u64 nMicrosSinceEpoch = CTimer::GetClockTicks();

	// Units of 100 microseconds
	return (nMicrosSinceEpoch - nStartTime ) / 100;
}

bool ParseInvitationPacket(const u8* pBuffer, size_t nSize, TAppleMIDIInvitation* pOutPacket)
{
	constexpr size_t nSizeWithoutName = sizeof(TAppleMIDIInvitation) - sizeof(TAppleMIDIInvitation::Name);
	if (nSize < nSizeWithoutName)
		return false;

	const u16 nSignature = ntohs(*(reinterpret_cast<const u16*>(pBuffer)));
	if (nSignature != AppleMIDISignature)
		return false;

	const u16 nCommand = ntohs(*(reinterpret_cast<const u16*>(pBuffer + 2)));
	if (nCommand != Invitation)
		return false;

	const u32 nVersion = ntohl(*(reinterpret_cast<const u32*>(pBuffer + 4)));
	if (nVersion != AppleMIDIVersion)
		return false;

	const u32 nInitiatorToken = ntohl(*(reinterpret_cast<const u32*>(pBuffer + 8)));
	const u32 nSSRC = ntohl(*(reinterpret_cast<const u32*>(pBuffer + 12)));

	pOutPacket->nSignature = nSignature;
	pOutPacket->nCommand = nCommand;
	pOutPacket->nVersion = nVersion;
	pOutPacket->nInitiatorToken = nInitiatorToken;
	pOutPacket->nSSRC = nSSRC;

	if (nSize > nSizeWithoutName)
		strncpy(pOutPacket->Name, reinterpret_cast<const char*>(pBuffer + 16), sizeof(pOutPacket->Name));
	else
		strncpy(pOutPacket->Name, "<unknown>", sizeof(pOutPacket->Name));

	return true;
}

bool ParseEndSessionPacket(const u8* pBuffer, size_t nSize, TAppleMIDIEndSession* pOutPacket)
{
	if (nSize < sizeof(TAppleMIDIEndSession))
		return false;

	const u16 nSignature = ntohs(*(reinterpret_cast<const u16*>(pBuffer)));
	if (nSignature != AppleMIDISignature)
		return false;

	const u16 nCommand = ntohs(*(reinterpret_cast<const u16*>(pBuffer + 2)));
	if (nCommand != EndSession)
		return false;

	const u32 nVersion = ntohl(*(reinterpret_cast<const u32*>(pBuffer + 4)));
	if (nVersion != AppleMIDIVersion)
		return false;

	const u32 nInitiatorToken = ntohl(*(reinterpret_cast<const u32*>(pBuffer + 8)));
	const u32 nSSRC = ntohl(*(reinterpret_cast<const u32*>(pBuffer + 12)));

	pOutPacket->nSignature = nSignature;
	pOutPacket->nCommand = nCommand;
	pOutPacket->nVersion = nVersion;
	pOutPacket->nInitiatorToken = nInitiatorToken;
	pOutPacket->nSSRC = nSSRC;

	return true;
}

bool ParseSyncPacket(const u8* pBuffer, size_t nSize, TAppleMIDISync* pOutPacket)
{
	if (nSize != sizeof(TAppleMIDISync))
		return false;

	const u32 nSignature = ntohs(*(reinterpret_cast<const u16*>(pBuffer)));
	if (nSignature != AppleMIDISignature)
		return false;

	const u32 nCommand = ntohs(*(reinterpret_cast<const u16*>(pBuffer + 2)));
	if (nCommand != Sync)
		return false;

	pOutPacket->nSignature = nSignature;
	pOutPacket->nCommand = nCommand;
	pOutPacket->nSSRC = ntohl(*(reinterpret_cast<const u32*>(pBuffer + 4)));
	pOutPacket->nCount = *(pBuffer + 8);
	pOutPacket->nTimestamps[0] = ntohll(*(reinterpret_cast<const u64*>(pBuffer + 12)));
	pOutPacket->nTimestamps[1] = ntohll(*(reinterpret_cast<const u64*>(pBuffer + 20)));
	pOutPacket->nTimestamps[2] = ntohll(*(reinterpret_cast<const u64*>(pBuffer + 28)));

	return true;
}

bool ParseMIDIPacket(const u8* pBuffer, size_t nSize, TRTPMIDI* pOutPacket, const u8** pOutMIDIData, size_t* pOutMIDIDataSize)
{
	const u16 nRTPFlags = ntohs(*(reinterpret_cast<const u16*>(pBuffer)));

	// Check version
	if (((nRTPFlags >> 14) & 0x03) != RTPMIDIVersion)
		return false;

	// Ensure no CSRC identifiers
	if (((nRTPFlags >> 8) & 0x0F) != 0)
		return false;

	// Check payload type
	if ((nRTPFlags & 0xFF) != RTPMIDIPayloadType)
		return false;

	const u16 nSequence = ntohs(*(reinterpret_cast<const u16*>(pBuffer + 2)));
	const u32 nTimestamp = ntohl(*(reinterpret_cast<const u32*>(pBuffer + 4)));
	const u32 nSSRC = ntohl(*(reinterpret_cast<const u32*>(pBuffer + 8)));

	pOutPacket->RTPHeader.nFlags = nRTPFlags;
	pOutPacket->RTPHeader.nSequence = nSequence;
	pOutPacket->RTPHeader.nTimestamp = nTimestamp;
	pOutPacket->RTPHeader.nSSRC = nSSRC;

	// RTP-MIDI variable-length header
	const u8 nHeader = *(pBuffer + 12);
	const u8* pMIDIData = pBuffer + 13;

	// Lower 4 bits of the header is is length
	u16 nMIDIDataSize = nHeader & 0x0F;

	// If B flag is set, length value is 12 bits
	if (nHeader & (1 << 7))
	{
		nMIDIDataSize <<= 8;
		nMIDIDataSize |= *pMIDIData;
		++pMIDIData;
	}

	const u8 nHead = *pMIDIData;
	const u8 nTail = *(pMIDIData + nMIDIDataSize - 1);

	// First segmented SysEx packet
	if (nHead == 0xF0 && nTail == 0xF0)
		--nMIDIDataSize;

	// Middle segmented SysEx packet
	else if (nHead == 0xF7 && nTail == 0xF0)
	{
		++pMIDIData;
		nMIDIDataSize -= 2;
	}

	// Last segmented SysEx packet
	else if (nHead == 0xF7 && nTail == 0xF7)
	{
		++pMIDIData;
		--nMIDIDataSize;
	}

	// Cancelled segmented SysEx packet
	else if (nHead == 0xF7 && nTail == 0xF4)
		nMIDIDataSize = 1;

	*pOutMIDIData = pMIDIData;
	*pOutMIDIDataSize = nMIDIDataSize;

	return true;
}

CAppleMIDIParticipant::CAppleMIDIParticipant(CBcmRandomNumberGenerator* pRandom)
	: CTask(TASK_STACK_SIZE, true),

	  m_pRandom(pRandom),

	  m_pControlSocket(nullptr),
	  m_pMIDISocket(nullptr),

	  m_nInitiatorControlPort(0),
	  m_nInitiatorMIDIPort(0),
	  m_ControlBuffer{0},
	  m_MIDIBuffer{0},

	  m_nControlResult(0),
	  m_nMIDIResult(0),

	  m_pReceiveHandler(nullptr),

	  m_State(TState::ControlInvitation),

	  m_nInitiatorToken(0),
	  m_nInitiatorSSRC(0),
	  m_nSSRC(0),
	  m_nLastMIDISequenceNumber(0),

	  m_nOffsetEstimate(0),
	  m_nLastSyncTime(0),

	  m_nSequence(0),
	  m_nLastFeedbackSequence(0),
	  m_nLastFeedbackTime(0)
{
}

CAppleMIDIParticipant::~CAppleMIDIParticipant()
{
	if (m_pControlSocket)
		delete m_pControlSocket;

	if (m_pMIDISocket)
		delete m_pControlSocket;
}

bool CAppleMIDIParticipant::Initialize()
{
	assert(m_pControlSocket == nullptr);
	assert(m_pMIDISocket == nullptr);

	CLogger* const pLogger    = CLogger::Get();
	CNetSubSystem* const pNet = CNetSubSystem::Get();

	if ((m_pControlSocket = new CSocket(pNet, IPPROTO_UDP)) == nullptr)
		return false;

	if ((m_pMIDISocket = new CSocket(pNet, IPPROTO_UDP)) == nullptr)
		return false;

	if (m_pControlSocket->Bind(ControlPort) != 0)
	{
		pLogger->Write(AppleMIDIName, LogError, "Couldn't bind to port %d", ControlPort);
		return false;
	}

	if (m_pMIDISocket->Bind(MIDIPort) != 0)
	{
		pLogger->Write(AppleMIDIName, LogError, "Couldn't bind to port %d", MIDIPort);
		return false;
	}

	// We started as a suspended task; run now that initialization is successful
	Start();

	return true;
}

void CAppleMIDIParticipant::Run()
{
	assert(m_pControlSocket != nullptr);
	assert(m_pMIDISocket != nullptr);

	CLogger* const pLogger       = CLogger::Get();
	CScheduler* const pScheduler = CScheduler::Get();

	while (true)
	{
		if ((m_nControlResult = m_pControlSocket->ReceiveFrom(m_ControlBuffer, sizeof(m_ControlBuffer), MSG_DONTWAIT, &m_InitiatorIPAddress, &m_nInitiatorControlPort)) < 0)
		{
			pLogger->Write(AppleMIDIName, LogError, "Control socket receive error: %d", m_nControlResult);
			return;
		}

		if ((m_nMIDIResult = m_pMIDISocket->ReceiveFrom(m_MIDIBuffer, sizeof(m_MIDIBuffer), MSG_DONTWAIT, &m_InitiatorIPAddress, &m_nInitiatorMIDIPort)) < 0)
		{
			pLogger->Write(AppleMIDIName, LogError, "MIDI socket receive error: %d", m_nMIDIResult);
			return;
		}

		switch (m_State)
		{
		case TState::ControlInvitation:
			ControlInvitationState();
			break;

		case TState::MIDIInvitation:
			MIDIInvitationState();
			break;

		case TState::Connected:
			ConnectedState();
			break;
		}

		// Allow other tasks to run
		pScheduler->Yield();
	}
}

void CAppleMIDIParticipant::ControlInvitationState()
{
	CLogger* const pLogger = CLogger::Get();
	TAppleMIDIInvitation InvitationPacket;

	if (m_nControlResult == 0)
		return;

	if (!ParseInvitationPacket(m_ControlBuffer, m_nControlResult, &InvitationPacket))
	{
		pLogger->Write(AppleMIDIName, LogError, "Unexpected packet");
		return;
	}

#ifdef APPLEMIDI_DEBUG
	pLogger->Write(AppleMIDIName, LogNotice, "<-- Control invitation");
#endif

	// Generate initiator token and accept
	m_nInitiatorToken = InvitationPacket.nInitiatorToken;
	m_nInitiatorSSRC = InvitationPacket.nSSRC;
	m_nSSRC = m_pRandom->GetNumber();

	if (!SendAcceptInvitationPacket(m_pControlSocket, m_nInitiatorControlPort))
	{
		pLogger->Write(AppleMIDIName, LogError, "Couldn't accept control invitation");
		return;
	}

	m_nLastSyncTime = GetSyncClock();
	m_State = TState::MIDIInvitation;
}

void CAppleMIDIParticipant::MIDIInvitationState()
{
	CLogger* const pLogger = CLogger::Get();
	TAppleMIDIInvitation InvitationPacket;

	if (m_nMIDIResult > 0)
	{
		if (!ParseInvitationPacket(m_MIDIBuffer, m_nMIDIResult, &InvitationPacket))
		{
			pLogger->Write(AppleMIDIName, LogError, "Unexpected packet");
			return;
		}

#ifdef APPLEMIDI_DEBUG
		pLogger->Write(AppleMIDIName, LogNotice, "<-- MIDI invitation");
		//DumpInvitationPacket(&InvitationPacket);
#endif

		if (SendAcceptInvitationPacket(m_pMIDISocket, m_nInitiatorMIDIPort))
		{
			CString IPAddressString;
			m_InitiatorIPAddress.Format(&IPAddressString);
			pLogger->Write(AppleMIDIName, LogNotice, "Connection to %s (%s) established", InvitationPacket.Name, static_cast<const char*>(IPAddressString));
			m_nLastSyncTime = GetSyncClock();
			m_State = TState::Connected;
		}
		else
		{
			pLogger->Write(AppleMIDIName, LogError, "Couldn't accept MIDI invitation");
			Reset();
		}
	}

	// Timeout
	else if ((GetSyncClock() - m_nLastSyncTime) > SyncTimeout)
	{
		pLogger->Write(AppleMIDIName, LogError, "MIDI port invitation timed out");
		Reset();
	}
}

void CAppleMIDIParticipant::ConnectedState()
{
	CLogger* const pLogger = CLogger::Get();

	TAppleMIDIEndSession EndSessionPacket;
	TRTPMIDI MIDIPacket;
	TAppleMIDISync SyncPacket;

	const u8* pMIDIData;
	size_t nMIDIDataSize;

	if (m_nControlResult > 0)
	{
		if (ParseEndSessionPacket(m_ControlBuffer, m_nControlResult, &EndSessionPacket))
		{
#ifdef APPLEMIDI_DEBUG
			pLogger->Write(AppleMIDIName, LogNotice, "<-- End session");
#endif

			if (EndSessionPacket.nSSRC == m_nInitiatorSSRC)
			{
				pLogger->Write(AppleMIDIName, LogNotice, "Initiator ended session");
				Reset();
				return;
			}
		}
	}

	if (m_nMIDIResult > 0)
	{
		if (ParseMIDIPacket(m_MIDIBuffer, m_nMIDIResult, &MIDIPacket, &pMIDIData, &nMIDIDataSize))
		{
			m_nSequence = MIDIPacket.RTPHeader.nSequence;
			if (m_pReceiveHandler)
				m_pReceiveHandler(pMIDIData, nMIDIDataSize);
		}
		else if (ParseSyncPacket(m_MIDIBuffer, m_nMIDIResult, &SyncPacket))
		{
#ifdef APPLEMIDI_DEBUG
			pLogger->Write(AppleMIDIName, LogNotice, "<-- Sync %d", SyncPacket.nCount);
			//DumpSyncPacket(&SyncPacket);
#endif

			if (SyncPacket.nSSRC == m_nInitiatorSSRC && (SyncPacket.nCount == 0 || SyncPacket.nCount == 2))
			{
				if (SyncPacket.nCount == 0)
					SendSyncPacket(SyncPacket.nTimestamps[0], GetSyncClock());
				else if (SyncPacket.nCount == 2)
				{
					m_nOffsetEstimate = ((SyncPacket.nTimestamps[2] + SyncPacket.nTimestamps[0]) / 2) - SyncPacket.nTimestamps[1];
					pLogger->Write(AppleMIDIName, LogNotice, "Offset estimate: %llu", m_nOffsetEstimate);
				}

				m_nLastSyncTime = GetSyncClock();
			}
			else
			{
				pLogger->Write(AppleMIDIName, LogError, "Unexpected sync packet");
			}
		}
	}

	const u64 nTicks = GetSyncClock();

	if ((nTicks - m_nLastFeedbackTime) > ReceiverFeedbackPeriod)
	{
		if (m_nSequence != m_nLastFeedbackSequence)
		{
			SendFeedbackPacket();
			m_nLastFeedbackSequence = m_nSequence;
		}
		m_nLastFeedbackTime = nTicks;
	}

	if ((nTicks - m_nLastSyncTime) > SyncTimeout)
	{
		pLogger->Write(AppleMIDIName, LogError, "Initiator timed out");
		Reset();
	}
}

void CAppleMIDIParticipant::Reset()
{
	m_State = TState::ControlInvitation;

	m_nInitiatorToken = 0;
	m_nInitiatorSSRC = 0;
	m_nSSRC = 0;
	m_nLastMIDISequenceNumber = 0;

	m_nOffsetEstimate = 0;
	m_nLastSyncTime = 0;

	m_nSequence = 0;
	m_nLastFeedbackSequence = 0;
	m_nLastFeedbackTime = 0;
}

bool CAppleMIDIParticipant::SendPacket(CSocket* pSocket, u16 nPort, const void* pData, size_t nSize)
{
	CLogger* const pLogger = CLogger::Get();

	const int nResult = pSocket->SendTo(pData, nSize, MSG_DONTWAIT, m_InitiatorIPAddress, nPort);

	if (nResult < 0)
	{
		pLogger->Write(AppleMIDIName, LogError, "Send failure, error code: %d", nResult);
		return false;
	}

	if (static_cast<size_t>(nResult) != nSize)
	{
		pLogger->Write(AppleMIDIName, LogError, "Send failure, only %d/%d bytes sent", nResult, nSize);
		return false;
	}

#ifdef APPLEMIDI_DEBUG
	pLogger->Write(AppleMIDIName, LogNotice, "Sent %d bytes to port %d", nResult, nPort);
#endif

	return true;
}

bool CAppleMIDIParticipant::SendAcceptInvitationPacket(CSocket* pSocket, u16 nPort)
{
	TAppleMIDIInvitation AcceptPacket =
	{
		htons(AppleMIDISignature),
		htons(InvitationAccepted),
		htonl(AppleMIDIVersion),
		htonl(m_nInitiatorToken),
		htonl(m_nSSRC),
		{'\0'}
	};

	// TODO: configurable name
	strncpy(AcceptPacket.Name, "mt32-pi", sizeof(AcceptPacket.Name));

#ifdef APPLEMIDI_DEBUG
	CLogger::Get()->Write(AppleMIDIName, LogNotice, "--> Accept invitation");
	//DumpInvitationPacket(&AcceptPacket);
#endif

	const size_t nSendSize = sizeof(AcceptPacket) - sizeof(AcceptPacket.Name) + strlen(AcceptPacket.Name) + 1;
	return SendPacket(m_pControlSocket, nPort, &AcceptPacket, nSendSize);
}

bool CAppleMIDIParticipant::SendSyncPacket(u64 nTimestamp1, u64 nTimestamp2)
{
	const TAppleMIDISync SyncPacket =
	{
		htons(AppleMIDISignature),
		htons(Sync),
		htonl(m_nSSRC),
		1,
		{ 0 },
		{
			htonll(nTimestamp1),
			htonll(nTimestamp2),
			0
		}
	};

#ifdef APPLEMIDI_DEBUG
	CLogger::Get()->Write(AppleMIDIName, LogNotice, "--> Sync 1");
	//DumpSyncPacket(&SyncPacket);
#endif

	return SendPacket(m_pControlSocket, m_nInitiatorMIDIPort, &SyncPacket, sizeof(SyncPacket));
}

bool CAppleMIDIParticipant::SendFeedbackPacket()
{
	const TAppleMIDIReceiverFeedback FeedbackPacket =
	{
		htons(AppleMIDISignature),
		htons(ReceiverFeedback),
		htonl(m_nSSRC),
		htonl(m_nSequence << 16)
	};

#ifdef APPLEMIDI_DEBUG
	CLogger::Get()->Write(AppleMIDIName, LogNotice, "--> Feedback");
	//DumpSyncPacket(&SyncPacket);
#endif

	return SendPacket(m_pControlSocket, m_nInitiatorMIDIPort, &FeedbackPacket, sizeof(FeedbackPacket));
}
