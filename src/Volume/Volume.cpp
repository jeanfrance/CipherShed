/*
 Copyright (c) 2008 TrueCrypt Foundation. All rights reserved.

 Governed by the TrueCrypt License 2.5 the full text of which is contained
 in the file License.txt included in TrueCrypt binary and source code
 distribution packages.
*/

#ifndef TC_WINDOWS
#include <errno.h>
#endif
#include "EncryptionModeLRW.h"
#include "EncryptionModeXTS.h"
#include "Volume.h"
#include "VolumeHeader.h"
#include "VolumeLayout.h"

namespace TrueCrypt
{
	Volume::Volume ()
		: HiddenVolumeProtectionTriggered (false),
		VolumeDataSize (0),
		TopWriteOffset (0),
		TotalDataRead (0),
		TotalDataWritten (0)
	{
	}

	Volume::~Volume ()
	{
	}

	void Volume::CheckProtectedRange (uint64 writeHostOffset, uint64 writeLength)
	{
		uint64 writeHostEndOffset = writeHostOffset + writeLength - 1;

		if ((writeHostOffset < ProtectedRangeStart) ? (writeHostEndOffset >= ProtectedRangeStart) : (writeHostOffset <= ProtectedRangeEnd - 1))
		{
			HiddenVolumeProtectionTriggered = true;
			throw VolumeProtected (SRC_POS);
		}
	}

	void Volume::Close ()
	{
		if (VolumeFile.get() == nullptr)
			throw NotInitialized (SRC_POS);
		
		VolumeFile.reset();
	}

	shared_ptr <EncryptionAlgorithm> Volume::GetEncryptionAlgorithm () const
	{
		if_debug (ValidateState ());
		return EA;
	}

	shared_ptr <EncryptionMode> Volume::GetEncryptionMode () const
	{
		if_debug (ValidateState ());
		return EA->GetMode();
	}

	void Volume::Open (const VolumePath &volumePath, bool preserveTimestamps, shared_ptr <VolumePassword> password, shared_ptr <KeyfileList> keyfiles, VolumeProtection::Enum protection, shared_ptr <VolumePassword> protectionPassword, shared_ptr <KeyfileList> protectionKeyfiles, bool sharedAccessAllowed, VolumeType::Enum volumeType, bool useBackupHeaders)
	{
		make_shared_auto (File, file);

		File::FileOpenFlags flags = (preserveTimestamps ? File::PreserveTimestamps : File::FlagsNone);

		try
		{
			if (protection == VolumeProtection::ReadOnly)
				file->Open (volumePath, File::OpenRead, File::ShareRead, flags);
			else
				file->Open (volumePath, File::OpenReadWrite, File::ShareNone, flags);
		}
		catch (SystemException &e)
		{
			if (e.GetErrorCode() == 
#ifdef TC_WINDOWS
				ERROR_SHARING_VIOLATION)
#else
				EAGAIN)
#endif
			{
				if (!sharedAccessAllowed)
					throw VolumeHostInUse (SRC_POS);

				file->Open (volumePath, protection == VolumeProtection::ReadOnly ? File::OpenRead : File::OpenReadWrite, File::ShareReadWriteIgnoreLock, flags);
			}
			else
				throw;
		}

		return Open (file, password, keyfiles, protection, protectionPassword, protectionKeyfiles, volumeType, useBackupHeaders);
	}

	void Volume::Open (shared_ptr <File> volumeFile, shared_ptr <VolumePassword> password, shared_ptr <KeyfileList> keyfiles, VolumeProtection::Enum protection, shared_ptr <VolumePassword> protectionPassword, shared_ptr <KeyfileList> protectionKeyfiles, VolumeType::Enum volumeType, bool useBackupHeaders)
	{
		if (!volumeFile)
			throw ParameterIncorrect (SRC_POS);

		Protection = protection;
		VolumeFile = volumeFile;

		try
		{
			VolumeHostSize = VolumeFile->Length();
			shared_ptr <VolumePassword> passwordKey = Keyfile::ApplyListToPassword (keyfiles, password);

			bool skipLayoutV1Normal = false;

			// Test volume layouts
			foreach (shared_ptr <VolumeLayout> layout, VolumeLayout::GetAvailableLayouts (volumeType))
			{
				if (skipLayoutV1Normal && typeid (*layout) == typeid (VolumeLayoutV1Normal))
				{
					// Skip VolumeLayoutV1Normal as it shares header location with VolumeLayoutV2Normal
					continue;
				}

				if (useBackupHeaders && !layout->HasBackupHeader())
					continue;

				int headerOffset = useBackupHeaders ? layout->GetBackupHeaderOffset() : layout->GetHeaderOffset();

				if (headerOffset >= 0)
					VolumeFile->SeekAt (headerOffset);
				else
					VolumeFile->SeekEnd (headerOffset);

				SecureBuffer headerBuffer (layout->GetHeaderSize());

				if (VolumeFile->Read (headerBuffer) != layout->GetHeaderSize())
					continue;

				EncryptionAlgorithmList layoutEncryptionAlgorithms = layout->GetSupportedEncryptionAlgorithms();
				EncryptionModeList layoutEncryptionModes = layout->GetSupportedEncryptionModes();

				if (typeid (*layout) == typeid (VolumeLayoutV2Normal))
				{
					skipLayoutV1Normal = true;

					// Test all algorithms and modes of VolumeLayoutV1Normal as it shares header location with VolumeLayoutV2Normal
					layoutEncryptionAlgorithms = EncryptionAlgorithm::GetAvailableAlgorithms();
					layoutEncryptionModes = EncryptionMode::GetAvailableModes();
				}

				shared_ptr <VolumeHeader> header = layout->GetHeader();

				if (header->Decrypt (headerBuffer, *passwordKey, layoutEncryptionAlgorithms, layoutEncryptionModes))
				{
					// Header decrypted

					if (typeid (*layout) == typeid (VolumeLayoutV2Normal) && header->GetRequiredMinProgramVersion() < 0x600)
					{
						// VolumeLayoutV1Normal has been opened as VolumeLayoutV2Normal
						layout.reset (new VolumeLayoutV1Normal);
						header->SetSize (layout->GetHeaderSize());
						layout->SetHeader (header);
					}

					Type = layout->GetType();
					SectorSize = header->GetSectorSize();
					VolumeDataOffset = layout->GetDataOffset (VolumeHostSize);
					VolumeDataSize = layout->GetDataSize (VolumeHostSize);

					Header = header;
					Layout = layout;
					EA = header->GetEncryptionAlgorithm();
					EncryptionMode &mode = *EA->GetMode();

					if (typeid (mode) == typeid (EncryptionModeLRW))
						mode.SetSectorOffset (VolumeDataOffset / SectorSize);

					// Volume protection
					if (Protection == VolumeProtection::HiddenVolumeReadOnly)
					{
						if (Type == VolumeType::Hidden)
							throw PasswordIncorrect (SRC_POS);
						else
						{
							try
							{
								Volume protectedVolume;

								protectedVolume.Open (VolumeFile,
									protectionPassword, protectionKeyfiles,
									VolumeProtection::ReadOnly,
									shared_ptr <VolumePassword> (), shared_ptr <KeyfileList> (),
									VolumeType::Hidden,
									useBackupHeaders);

								if (protectedVolume.GetType() != VolumeType::Hidden)
									ParameterIncorrect (SRC_POS);

								ProtectedRangeStart = protectedVolume.VolumeDataOffset;
								ProtectedRangeEnd = protectedVolume.VolumeDataOffset + protectedVolume.VolumeDataSize;

								if (typeid (*protectedVolume.Layout) == typeid (VolumeLayoutV1Hidden))
									ProtectedRangeEnd += protectedVolume.Layout->GetHeaderSize();
							}
							catch (PasswordException&)
							{
								if (protectionKeyfiles && !protectionKeyfiles->empty())
									throw ProtectionPasswordKeyfilesIncorrect (SRC_POS);
								throw ProtectionPasswordIncorrect (SRC_POS);
							}
						}
					}
					return;
				}
			}
			if (keyfiles && !keyfiles->empty())
				throw PasswordKeyfilesIncorrect (SRC_POS);
			throw PasswordIncorrect (SRC_POS);
		}
		catch (...)
		{
			Close();
			throw;
		}
	}

	void Volume::ReadSectors (const BufferPtr &buffer, uint64 byteOffset)
	{
		if_debug (ValidateState ());

		uint64 length = buffer.Size();
		uint64 hostOffset = VolumeDataOffset + byteOffset;

		if (length % SectorSize != 0 || byteOffset % SectorSize != 0)
			throw ParameterIncorrect (SRC_POS);

		if (VolumeFile->ReadAt (buffer, hostOffset) != length)
			throw MissingVolumeData (SRC_POS);

		EA->DecryptSectors (buffer, hostOffset / SectorSize, length / SectorSize, SectorSize);

		TotalDataRead += length;
	}

	void Volume::ReEncryptHeader (bool backupHeader, const ConstBufferPtr &newSalt, const ConstBufferPtr &newHeaderKey, shared_ptr <Pkcs5Kdf> newPkcs5Kdf)
	{
		if_debug (ValidateState ());
		
		if (Protection == VolumeProtection::ReadOnly)
			throw VolumeReadOnly (SRC_POS);

		SecureBuffer newHeaderBuffer (Layout->GetHeaderSize());
		
		Header->EncryptNew (newHeaderBuffer, newSalt, newHeaderKey, newPkcs5Kdf);

		int headerOffset = backupHeader ? Layout->GetBackupHeaderOffset() : Layout->GetHeaderOffset();

		if (headerOffset >= 0)
			VolumeFile->SeekAt (headerOffset);
		else
			VolumeFile->SeekEnd (headerOffset);

		VolumeFile->Write (newHeaderBuffer);
	}

	void Volume::ValidateState () const
	{
		if (VolumeFile.get() == nullptr)
			throw NotInitialized (SRC_POS);
	}

	void Volume::WriteSectors (const ConstBufferPtr &buffer, uint64 byteOffset)
	{
		if_debug (ValidateState ());

		uint64 length = buffer.Size();
		uint64 hostOffset = VolumeDataOffset + byteOffset;

		if (length % SectorSize != 0
			|| byteOffset % SectorSize != 0
			|| byteOffset + length > VolumeDataSize)
			throw ParameterIncorrect (SRC_POS);

		if (Protection == VolumeProtection::ReadOnly)
			throw VolumeReadOnly (SRC_POS);

		if (HiddenVolumeProtectionTriggered)
			throw VolumeProtected (SRC_POS);

		if (Protection == VolumeProtection::HiddenVolumeReadOnly)
			CheckProtectedRange (hostOffset, length);

		SecureBuffer encBuf (buffer.Size());
		encBuf.CopyFrom (buffer);

		EA->EncryptSectors (encBuf, hostOffset / SectorSize, length / SectorSize, SectorSize);
		VolumeFile->WriteAt (encBuf, hostOffset);

		TotalDataWritten += length;
		
		uint64 writeEndOffset = byteOffset + buffer.Size();
		if (writeEndOffset > TopWriteOffset)
			TopWriteOffset = writeEndOffset;
	}
}