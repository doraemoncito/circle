//
// usbaudiostreaming.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2022  R. Stange <rsta2@o2online.de>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <circle/usb/usbaudiostreaming.h>
#include <circle/usb/usbaudiocontrol.h>
#include <circle/usb/usbaudiofunctopology.h>
#include <circle/usb/usbaudio.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/devicenameservice.h>
#include <circle/debug.h>
#include <circle/util.h>
#include <assert.h>

// supported format
#define CHANNELS		2		// Stereo
#define SUBFRAME_SIZE		2		// 16-bit signed
#define CHUNK_FREQUENCY		1000		// per second

// convert 3-byte sample rate value to an unsigned
#define RATE2UNSIGNED(rate)	(  (unsigned) (rate)[0]		\
				 | (unsigned) (rate)[1] << 8	\
				 | (unsigned) (rate)[2] << 16)

static const char DeviceNamePattern[] = "uaudio%u-%u";

CUSBAudioStreamingDevice::CUSBAudioStreamingDevice (CUSBFunction *pFunction)
:	CUSBFunction (pFunction),
	m_pEndpointOut (nullptr),
	m_pEndpointSync (nullptr),
	m_nSampleRate (0),
	m_nChunkSizeBytes (0),
	m_nPacketsPerChunk (0),
	m_bSyncEPActive (FALSE),
	m_nSyncAccu (0),
	m_uchClockSourceID (USB_AUDIO_UNDEFINED_UNIT_ID),
	m_uchFeatureUnitID (USB_AUDIO_UNDEFINED_UNIT_ID),
	From ("uaudio")
{
	memset (&m_DeviceInfo, 0, sizeof m_DeviceInfo);
}

CUSBAudioStreamingDevice::~CUSBAudioStreamingDevice (void)
{
	if (m_DeviceName.GetLength ())
	{
		CDeviceNameService::Get ()->RemoveDevice (m_DeviceName, FALSE);
	}

	delete m_pEndpointSync;
	m_pEndpointSync = nullptr;

	delete m_pEndpointOut;
	m_pEndpointOut = nullptr;
}

boolean CUSBAudioStreamingDevice::Initialize (void)
{
	if (!CUSBFunction::Initialize ())
	{
		return FALSE;
	}

	return GetNumEndpoints () >= 1;		// ignore no-endpoint interfaces
}

boolean CUSBAudioStreamingDevice::Configure (void)
{
	assert (GetNumEndpoints () >= 1);

	m_bVer200 = GetInterfaceProtocol () == USB_PROTO_AUDIO_VER_200;

	CUSBAudioStreamingInterfaceDescriptor *pGeneralDesc;
	while ((pGeneralDesc = (CUSBAudioStreamingInterfaceDescriptor *)
					GetDescriptor (DESCRIPTOR_CS_INTERFACE)) != nullptr)
	{
		if (pGeneralDesc->bDescriptorSubtype == USB_AUDIO_STREAMING_GENERAL)
		{
			break;
		}
	}

	if (!pGeneralDesc)
	{
		LOGWARN ("AS_GENERAL descriptor expected");

		return FALSE;
	}

	TUSBAudioTypeIFormatTypeDescriptor *pFormatTypeDesc =
		(TUSBAudioTypeIFormatTypeDescriptor *) GetDescriptor (DESCRIPTOR_CS_INTERFACE);
	if (   !pFormatTypeDesc
	    || pFormatTypeDesc->bDescriptorSubtype != USB_AUDIO_FORMAT_TYPE)
	{
		LOGWARN ("FORMAT_TYPE descriptor expected");

		return FALSE;
	}

	TUSBAudioEndpointDescriptor *pEndpointDesc;
	pEndpointDesc = (TUSBAudioEndpointDescriptor *) GetDescriptor (DESCRIPTOR_ENDPOINT);
	if (   !pEndpointDesc
	    || (pEndpointDesc->bmAttributes     & 0x33) != 0x01	 // Isochronous, Data
	    || (pEndpointDesc->bEndpointAddress & 0x80) != 0x00) // Output EP
	{
		LOGWARN ("Isochronous data output EP expected");

		return FALSE;
	}

	if (pEndpointDesc->bInterval != 1)			// TODO
	{
		LOGWARN ("Unsupported EP timing (%u)", (unsigned) pEndpointDesc->bInterval);

		return FALSE;
	}

	if (!m_bVer200)
	{
		if (   pFormatTypeDesc->bFormatType           != USB_AUDIO_FORMAT_TYPE_I
		    || pFormatTypeDesc->Ver100.bNrChannels    != CHANNELS
		    || pFormatTypeDesc->Ver100.bSubframeSize  != SUBFRAME_SIZE
		    || pFormatTypeDesc->Ver100.bBitResolution != SUBFRAME_SIZE*8)
		{
			LOGWARN ("Invalid output format");
#ifndef NDEBUG
			debug_hexdump (pFormatTypeDesc, pFormatTypeDesc->bLength, From);
#endif

			return FALSE;
		}
	}
	else
	{
		if (   pFormatTypeDesc->bFormatType           != USB_AUDIO_FORMAT_TYPE_I
		    || pFormatTypeDesc->Ver200.bSubslotSize   != SUBFRAME_SIZE
		    || pFormatTypeDesc->Ver200.bBitResolution != SUBFRAME_SIZE*8
		    || pGeneralDesc->Ver200.bNrChannels       != CHANNELS)
		{
			LOGWARN ("Invalid output format (chans %u)",
				 (unsigned) pGeneralDesc->Ver200.bNrChannels);
#ifndef NDEBUG
			debug_hexdump (pFormatTypeDesc, pFormatTypeDesc->bLength, From);
#endif

			return FALSE;
		}
	}

	if ((pEndpointDesc->bmAttributes & 0x0C) == 0x04)		// Asynchronous sync
	{
		TUSBAudioEndpointDescriptor *pEndpointInDesc;
		pEndpointInDesc = (TUSBAudioEndpointDescriptor *) GetDescriptor (DESCRIPTOR_ENDPOINT);
		if (   !pEndpointInDesc
		    || (pEndpointInDesc->bmAttributes     & 0x3F) != 0x11  // Isochronous, Feedback
		    || (pEndpointInDesc->bEndpointAddress & 0x80) != 0x80) // Input EP
		{
			LOGWARN ("Isochronous feedback input EP expected");

			return FALSE;
		}

		m_pEndpointSync = new CUSBEndpoint (GetDevice (), (TUSBEndpointDescriptor *) pEndpointInDesc);
		assert (m_pEndpointSync != 0);
	}

	m_bSynchronousSync = (pEndpointDesc->bmAttributes & 0x0C) == 0x0C;

	m_pEndpointOut = new CUSBEndpoint (GetDevice (), (TUSBEndpointDescriptor *) pEndpointDesc);
	assert (m_pEndpointOut != 0);

	if (!CUSBFunction::Configure ())
	{
		LOGWARN ("Cannot set interface");

		return FALSE;
	}

	// Interface of audio control device is the first in configuration descriptor,
	// so that the respective function has the index 0.
	CUSBFunction *pFunction = GetDevice ()->GetFunction (0);
	CUSBAudioControlDevice *pControlDevice = (CUSBAudioControlDevice *) pFunction;
	if (   !pFunction
	    || pControlDevice->GetInterfaceClass ()    != 1
	    || pControlDevice->GetInterfaceSubClass () != 1)
	{
		LOGWARN ("Associated control device not found");

		return FALSE;
	}

	if (!m_bVer200)
	{
		m_DeviceInfo.TerminalType =
			pControlDevice->GetTerminalType (pGeneralDesc->Ver100.bTerminalLink);

		// fetch format info from descriptor
		if (pFormatTypeDesc->Ver100.bSamFreqType == 0)
		{
			// continuous range
			m_DeviceInfo.SampleRateRanges = 1;
			m_DeviceInfo.SampleRateRange[0].Min =
				RATE2UNSIGNED (pFormatTypeDesc->Ver100.tSamFreq[0]);
			m_DeviceInfo.SampleRateRange[0].Max =
				RATE2UNSIGNED (pFormatTypeDesc->Ver100.tSamFreq[1]);
		}
		else
		{
			// discrete sample rates
			unsigned nSampleRates = pFormatTypeDesc->Ver100.bSamFreqType;
			if (nSampleRates > MaxSampleRatesRanges)
			{
				nSampleRates = MaxSampleRatesRanges;
			}
			m_DeviceInfo.SampleRateRanges = nSampleRates;

			for (unsigned i = 0; i < nSampleRates; i++)
			{
				m_DeviceInfo.SampleRateRange[i].Min =
				m_DeviceInfo.SampleRateRange[i].Max =
					RATE2UNSIGNED (pFormatTypeDesc->Ver100.tSamFreq[i]);
			}
		}

		// get access to the Feature Unit, to control volume etc.
		m_uchFeatureUnitID =
			pControlDevice->GetFeatureUnitID (pGeneralDesc->Ver100.bTerminalLink);
		if (   m_uchFeatureUnitID != USB_AUDIO_UNDEFINED_UNIT_ID
		    && pControlDevice->IsControlSupported (m_uchFeatureUnitID, 1,
							   CUSBAudioFeatureUnit::VolumeControl)
		    && pControlDevice->IsControlSupported (m_uchFeatureUnitID, 2,
							   CUSBAudioFeatureUnit::VolumeControl))
		{
			// get volume range from left channel only, should be same as right
			DMA_BUFFER (s16, VolumeBuffer, 1);
			if (GetHost ()->ControlMessage (GetEndpoint0 (),
							REQUEST_IN | REQUEST_CLASS | REQUEST_TO_INTERFACE,
							USB_AUDIO_REQ_GET_MIN,
							USB_AUDIO_FU_VOLUME_CONTROL << 8 | 0x01, // left
							m_uchFeatureUnitID << 8,
							VolumeBuffer, 2) < 0)
			{
				LOGWARN ("Cannot get volume minimum");

				return FALSE;
			}

			m_DeviceInfo.MinVolume = VolumeBuffer[0] >> 8;

			if (GetHost ()->ControlMessage (GetEndpoint0 (),
							REQUEST_IN | REQUEST_CLASS | REQUEST_TO_INTERFACE,
							USB_AUDIO_REQ_GET_MAX,
							USB_AUDIO_FU_VOLUME_CONTROL << 8 | 0x01, // left
							m_uchFeatureUnitID << 8,
							VolumeBuffer, 2) < 0)
			{
				LOGWARN ("Cannot get volume maximum");

				return FALSE;
			}

			m_DeviceInfo.MaxVolume = VolumeBuffer[0] >> 8;

			m_DeviceInfo.VolumeSupported = TRUE;
		}
	}
	else
	{
		m_DeviceInfo.TerminalType =
			pControlDevice->GetTerminalType (pGeneralDesc->Ver200.bTerminalLink);

		// request clock source ID for this Input Terminal
		m_uchClockSourceID =
			pControlDevice->GetClockSourceID (pGeneralDesc->Ver200.bTerminalLink);
		if (m_uchClockSourceID == USB_AUDIO_UNDEFINED_UNIT_ID)
		{
			LOGWARN ("Associated clock source not found (%u)",
				 (unsigned) pGeneralDesc->Ver200.bTerminalLink);

			return FALSE;
		}

		// fetch supported sampling frequency ranges from clock source,
		// number of ranges first
		DMA_BUFFER (u16, NumSubRanges, 1);
		if (GetHost ()->ControlMessage (GetEndpoint0 (),
						REQUEST_IN | REQUEST_CLASS | REQUEST_TO_INTERFACE,
						USB_AUDIO_REQ_RANGE,
						USB_AUDIO_CS_SAM_FREQ_CONTROL << 8,
						m_uchClockSourceID << 8,
						NumSubRanges, 2) < 0)
		{
			LOGWARN ("Cannot get number of sampling frequency subranges");

			return FALSE;
		}

		// now that we know the number of ranges, request the whole parameter block
		unsigned nSampleRates = NumSubRanges[0];
		unsigned nBufferSize = 2 + 12*nSampleRates;
		DMA_BUFFER (u8, RangesBuffer, nBufferSize);
		if (GetHost ()->ControlMessage (GetEndpoint0 (),
						REQUEST_IN | REQUEST_CLASS | REQUEST_TO_INTERFACE,
						USB_AUDIO_REQ_RANGE,
						USB_AUDIO_CS_SAM_FREQ_CONTROL << 8,
						m_uchClockSourceID << 8,
						RangesBuffer, nBufferSize) < 0)
		{
			LOGWARN ("Cannot get sampling frequency ranges");

			return FALSE;
		}

		// fill in the m_DeviceInfo struct
		if (nSampleRates > MaxSampleRatesRanges)
		{
			nSampleRates = MaxSampleRatesRanges;
		}
		m_DeviceInfo.SampleRateRanges = nSampleRates;

		u32 *pFreq = (u32 *) (RangesBuffer+2);
		for (unsigned i = 0; i < nSampleRates; i++)
		{
			m_DeviceInfo.SampleRateRange[i].Min = *pFreq++;
			m_DeviceInfo.SampleRateRange[i].Max = *pFreq++;
			m_DeviceInfo.SampleRateRange[i].Resolution = *pFreq++;
		}

		// get access to the Feature Unit, to control volume etc.
		m_uchFeatureUnitID =
			pControlDevice->GetFeatureUnitID (pGeneralDesc->Ver200.bTerminalLink);
		if (   m_uchFeatureUnitID != USB_AUDIO_UNDEFINED_UNIT_ID
		    && pControlDevice->IsControlSupported (m_uchFeatureUnitID, 1,
							   CUSBAudioFeatureUnit::VolumeControl)
		    && pControlDevice->IsControlSupported (m_uchFeatureUnitID, 2,
							   CUSBAudioFeatureUnit::VolumeControl))
		{
			// get volume range from left channel only, should be same as right
			DMA_BUFFER (s16, VolumeBuffer, 4);
			if (GetHost ()->ControlMessage (GetEndpoint0 (),
							REQUEST_IN | REQUEST_CLASS | REQUEST_TO_INTERFACE,
							USB_AUDIO_REQ_RANGE,
							USB_AUDIO_FU_VOLUME_CONTROL << 8 | 0x01, // left
							m_uchFeatureUnitID << 8,
							VolumeBuffer, 8) < 0)
			{
				LOGWARN ("Cannot get volume range");

				return FALSE;
			}

			if (VolumeBuffer[0] == 1)
			{
				m_DeviceInfo.MinVolume = VolumeBuffer[1] >> 8;
				m_DeviceInfo.MaxVolume = VolumeBuffer[2] >> 8;
				m_DeviceInfo.VolumeSupported = TRUE;
			}
		}
	}

	m_DeviceInfo.MuteSupported =    m_uchFeatureUnitID != USB_AUDIO_UNDEFINED_UNIT_ID
				     && pControlDevice->IsControlSupported (m_uchFeatureUnitID, 0,
						CUSBAudioFeatureUnit::MuteControl);

	// write supported sample rates info to log
	CString SampleRates;
	for (unsigned i = 0; i < m_DeviceInfo.SampleRateRanges; i++)
	{
		CString String;
		if (   m_DeviceInfo.SampleRateRange[i].Min
		    != m_DeviceInfo.SampleRateRange[i].Max)
		{
			// continuous subrange
			String.Format ("%u-%u/%u",
					m_DeviceInfo.SampleRateRange[i].Min,
					m_DeviceInfo.SampleRateRange[i].Max,
					m_DeviceInfo.SampleRateRange[i].Resolution);
		}
		else
		{
			// discrete rate
			String.Format ("%u", m_DeviceInfo.SampleRateRange[i].Min);
		}

		if (i)
		{
			SampleRates.Append (", ");
		}

		SampleRates.Append (String);
	}

	m_DeviceName.Format (DeviceNamePattern,
			     pControlDevice->GetDeviceNumber (),
			     pControlDevice->GetNextStreamingSubDeviceNumber ());

	CDeviceNameService::Get ()->AddDevice (m_DeviceName, this, FALSE);

	From = m_DeviceName;	// for logger

	LOGNOTE ("Terminal type is 0x%X", m_DeviceInfo.TerminalType);
	LOGNOTE ("Supported sample rate(s): %s Hz", (const char *) SampleRates);

	return TRUE;
}

CUSBAudioStreamingDevice::TDeviceInfo CUSBAudioStreamingDevice::GetDeviceInfo (void) const
{
	return m_DeviceInfo;
}

boolean CUSBAudioStreamingDevice::Setup (unsigned nSampleRate)
{
	// is sample rate supported?
	unsigned i;
	for (i = 0; i < m_DeviceInfo.SampleRateRanges; i++)
	{
		if (   m_DeviceInfo.SampleRateRange[i].Min >= nSampleRate
		    && m_DeviceInfo.SampleRateRange[i].Max <= nSampleRate)
		{
			break;
		}
	}

	if (i >= m_DeviceInfo.SampleRateRanges)
	{
		LOGWARN ("Sample rate is not supported (%u)", nSampleRate);

		return FALSE;
	}

	DMA_BUFFER (u32, tSampleFreq, 1);
	tSampleFreq[0] = nSampleRate;
	if (!m_bVer200)
	{
		if (GetHost ()->ControlMessage (GetEndpoint0 (),
						REQUEST_OUT | REQUEST_CLASS | REQUEST_TO_ENDPOINT,
						USB_AUDIO_REQ_SET_CUR,
						USB_AUDIO_CS_SAM_FREQ_CONTROL << 8,
						m_pEndpointOut->GetNumber (),
						tSampleFreq, 3) < 0)
		{
			LOGDBG ("Cannot set sample rate");

			return FALSE;
		}
	}
	else
	{
		assert (m_uchClockSourceID != USB_AUDIO_UNDEFINED_UNIT_ID);
		if (GetHost ()->ControlMessage (GetEndpoint0 (),
						REQUEST_OUT | REQUEST_CLASS | REQUEST_TO_INTERFACE,
						USB_AUDIO_REQ_SET_CUR,
						USB_AUDIO_CS_SAM_FREQ_CONTROL << 8,
						m_uchClockSourceID << 8,
						tSampleFreq, 4) < 0)
		{
			LOGDBG ("Cannot set sample rate");

			return FALSE;
		}
	}

	m_nSampleRate = nSampleRate;

	if (m_bSynchronousSync)
	{
		UpdateChunkSize ();
	}
	else
	{
		m_nChunkSizeBytes = nSampleRate * CHANNELS * SUBFRAME_SIZE / CHUNK_FREQUENCY;
	}

	return TRUE;
}

unsigned CUSBAudioStreamingDevice::GetChunkSizeBytes (void) const
{
	assert (m_nChunkSizeBytes);
	return m_nChunkSizeBytes;
}

boolean CUSBAudioStreamingDevice::SendChunk (const void *pBuffer, unsigned nChunkSizeBytes,
					     TCompletionRoutine *pCompletionRoutine, void *pParam)
{
	assert (pBuffer);

	assert (m_pEndpointOut);
	CUSBRequest *pURB = new CUSBRequest (m_pEndpointOut, (void *) pBuffer, nChunkSizeBytes);
	assert (pURB);

	if (m_bSynchronousSync)
	{
		assert (m_nPacketsPerChunk > 0);
		for (unsigned i = 0; i < m_nPacketsPerChunk; i++)
		{
			pURB->AddIsoPacket (m_usPacketSizeBytes[i]);
		}
	}
	else
	{
		pURB->AddIsoPacket (nChunkSizeBytes);
	}

	pURB->SetCompletionRoutine (CompletionHandler, pParam, (void *) pCompletionRoutine);

	boolean bOK = GetHost ()->SubmitAsyncRequest (pURB);

	if (   bOK
	    && m_pEndpointSync
	    && !m_bSyncEPActive)
	{
		m_bSyncEPActive = TRUE;

		u16 usPacketSize = GetDevice ()->GetSpeed () == USBSpeedFull ? 3 : 4;

		assert (m_pEndpointSync);
		CUSBRequest *pURBSync = new CUSBRequest (m_pEndpointSync, &m_SyncEPBuffer,
							 usPacketSize);
		assert (pURBSync);

		pURBSync->AddIsoPacket (usPacketSize);

		pURBSync->SetCompletionRoutine (SyncCompletionHandler, nullptr, this);

		bOK = GetHost ()->SubmitAsyncRequest (pURBSync);
	}
	else if (   bOK
		 && m_bSynchronousSync)
	{
		UpdateChunkSize ();
	}

	return bOK;
}

boolean CUSBAudioStreamingDevice::SetMute (boolean bEnable)
{
	if (!m_DeviceInfo.MuteSupported)
	{
		return FALSE;
	}

	assert (m_uchFeatureUnitID != USB_AUDIO_UNDEFINED_UNIT_ID);

	DMA_BUFFER (u8, MuteBuffer, 1);
	MuteBuffer[0] = bEnable ? 1 : 0;

	// same request for v1.00 and v2.00
	if (GetHost ()->ControlMessage (GetEndpoint0 (),
					REQUEST_OUT | REQUEST_CLASS | REQUEST_TO_INTERFACE,
					USB_AUDIO_REQ_SET_CUR,
					USB_AUDIO_FU_MUTE_CONTROL << 8 | 0x00,	// master channel
					m_uchFeatureUnitID << 8,
					MuteBuffer, 1) < 0)
	{
		return FALSE;
	}

	return TRUE;
}

boolean CUSBAudioStreamingDevice::SetVolume (unsigned nChannel, int ndB)
{
	assert (nChannel <= 1);

	if (!m_DeviceInfo.VolumeSupported)
	{
		return FALSE;
	}

	assert (m_uchFeatureUnitID != USB_AUDIO_UNDEFINED_UNIT_ID);

	DMA_BUFFER (s16, VolumeBuffer, 1);
	VolumeBuffer[0] = ndB << 8;

	// same request for v1.00 and v2.00
	if (GetHost ()->ControlMessage (GetEndpoint0 (),
					REQUEST_OUT | REQUEST_CLASS | REQUEST_TO_INTERFACE,
					USB_AUDIO_REQ_SET_CUR,
					USB_AUDIO_FU_VOLUME_CONTROL << 8 | (nChannel+1),
					m_uchFeatureUnitID << 8,
					VolumeBuffer, 2) < 0)
	{
		return FALSE;
	}

	return TRUE;
}

void CUSBAudioStreamingDevice::CompletionHandler (CUSBRequest *pURB, void *pParam, void *pContext)
{
	assert (pURB);
	delete pURB;

	TCompletionRoutine *pCompletionRoutine = (TCompletionRoutine *) pContext;
	if (pCompletionRoutine)
	{
		(*pCompletionRoutine) (pParam);
	}
}

void CUSBAudioStreamingDevice::SyncCompletionHandler (CUSBRequest *pURB, void *pParam,
						      void *pContext)
{
	CUSBAudioStreamingDevice *pThis = (CUSBAudioStreamingDevice *) pContext;
	assert (pThis);

	assert (pURB);
	boolean bOK = !!pURB->GetStatus ();
	boolean bFormat10_14 = bOK && pURB->GetResultLength () == 3;

	delete pURB;

	assert (pThis->m_bSyncEPActive);

	if (bOK)
	{
		if (bFormat10_14)
		{
			// Q10.14 format (FS)
			pThis->m_nSyncAccu += pThis->m_SyncEPBuffer[0] & 0xFFFFFF;
			pThis->m_nChunkSizeBytes =   (pThis->m_nSyncAccu >> 14)
						   * CHANNELS * SUBFRAME_SIZE;
			pThis->m_nSyncAccu &= 0x3FFF;
		}
		else
		{
			// Q16.16 format (HS)
			pThis->m_nSyncAccu += pThis->m_SyncEPBuffer[0];
			pThis->m_nChunkSizeBytes =   (pThis->m_nSyncAccu >> 16)
						   * CHANNELS * SUBFRAME_SIZE;
			pThis->m_nSyncAccu &= 0xFFFF;
		}
	}

	pThis->m_bSyncEPActive = FALSE;
}

void CUSBAudioStreamingDevice::UpdateChunkSize (void)
{
	assert (m_bSynchronousSync);
	assert (m_nSampleRate > 0);

	unsigned nUSBFrameRate = GetDevice ()->GetSpeed () == USBSpeedFull ? 1000 : 8000;

	m_SpinLock.Acquire ();

	m_nPacketsPerChunk = nUSBFrameRate / CHUNK_FREQUENCY;

	unsigned nChunkSizeBytes = 0;
	for (unsigned i = 0; i < m_nPacketsPerChunk; i++)
	{
		m_nSyncAccu += m_nSampleRate;
		unsigned nFrames = m_nSyncAccu / nUSBFrameRate;
		m_nSyncAccu %= nUSBFrameRate;

		m_usPacketSizeBytes[i] = nFrames * CHANNELS * SUBFRAME_SIZE;

		nChunkSizeBytes += m_usPacketSizeBytes[i];
	}

	m_nChunkSizeBytes = nChunkSizeBytes;

	m_SpinLock.Release ();
}