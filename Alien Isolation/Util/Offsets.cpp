#include "Util.h"
#include "../Main.h"

#include <boost/assign.hpp>
#include <fstream>
#include <unordered_map>
#include <Psapi.h>

using namespace boost::assign;

namespace
{
  bool m_UseScannedResults = false;
  std::unordered_map<std::string, util::offsets::Signature> m_Signatures;

  // The Steam, GOG and Epic Games Store releases of the game run the same code, but each was built separately, so
  // their code and data sit at different addresses. Each offset is listed once per release.
  util::offsets::GameBuild m_Build = util::offsets::GameBuild::Unknown;

  struct BuildOffsets
  {
    int Steam;
    int GOG;
    int Epic;
  };

  // Fill with hardcoded offsets if you don't want to use scanning
  // These should be relative to the module base.
  //                                                         Steam      GOG        Epic Games Store
  std::unordered_map<std::string, BuildOffsets> m_HardcodedOffsets = map_list_of
  ("OFFSET_D3D",                 BuildOffsets{ 0x17DF5CC, 0x17F980C, 0x17EF77C })
  ("OFFSET_MAIN",                BuildOffsets{ 0x12F0C88, 0x130AEC8, 0x130D1A8 })

  ("OFFSET_CAMERAUPDATE",        BuildOffsets{ 0x0032300, 0x00321C0, 0x0041CC0 })
  ("OFFSET_GETCAMERAMATRIX",     BuildOffsets{ 0x05B0B40, 0x05B0D40, 0x0502C60 })
  ("OFFSET_POVMANAGER",          BuildOffsets{ 0x1366A00, 0x1380C40, 0x1376BB0 })
  ("OFFSET_POSTPROCESSUPDATE",   BuildOffsets{ 0x0608C50, 0x0608D70, 0x055B0B0 })
  ("OFFSET_TONEMAPUPDATE",       BuildOffsets{ 0x0208490, 0x02086F0, 0x0218970 })

  ("OFFSET_INPUTUPDATE",         BuildOffsets{ 0x057D6C0, 0x057D640, 0x04CF4D0 })
  ("OFFSET_GAMEPADUPDATE",       BuildOffsets{ 0x060EE30, 0x060F130, 0x0561300 })
  ("OFFSET_COMBATMANAGERUPDATE", BuildOffsets{ 0x037A800, 0x037A9B0, 0x038B340 })

  ("OFFSET_SHOWMOUSE",           BuildOffsets{ 0x1359B44, 0x1373D84, 0x1369CF4 })
  ("OFFSET_DRAWUI",              BuildOffsets{ 0x1240F27, 0x125AF27, 0x125CEE7 })
  ("OFFSET_FREEZETIME",          BuildOffsets{ 0x12F194C, 0x130BB8C, 0x130DE6C })
  ("OFFSET_SCALEFORM",           BuildOffsets{ 0x134A78C, 0x13649CC, 0x1366CAC })
  ("OFFSET_TIMESCALE",           BuildOffsets{ 0x0DC6EA0, 0x0DDDEF0, 0x0DDDF90 })
  ("OFFSET_POSTPROCESS",         BuildOffsets{ 0x15D0970, 0x15EABB0, 0x15E0B20 })
  ("OFFSET_GLOWPATCH",           BuildOffsets{ 0x03A3494, 0x03A3594, 0x03B4254 });

  bool DataCompare(BYTE* pData, BYTE* bSig, const char* szMask)
  {
    for (; *szMask; ++szMask, ++pData, ++bSig)
    {
      if (*szMask == 'x' && *pData != *bSig)
        return false;
    }
    return (*szMask) == 0;
  }

  BYTE* FindPattern(BYTE* dwAddress, __int64 dwSize, BYTE* pbSig, const char* szMask)
  {
    register BYTE bFirstByte = *(BYTE*)pbSig;

    __int64 length = (__int64)dwAddress + dwSize - strlen(szMask);

    for (register __int64 i = (__int64)dwAddress; i < length; i += 4) // might run faster with 8 bytes but I am too lazy
    {
      unsigned x = *(unsigned*)(i);

      if ((x & 0xFF) == bFirstByte)
        if (DataCompare(reinterpret_cast<BYTE*>(i), pbSig, szMask))
          return reinterpret_cast<BYTE*>(i);

      if ((x & 0xFF00) >> 8 == bFirstByte)
        if (DataCompare(reinterpret_cast<BYTE*>(i + 1), pbSig, szMask))
          return reinterpret_cast<BYTE*>(i + 1);

      if ((x & 0xFF0000) >> 16 == bFirstByte)
        if (DataCompare(reinterpret_cast<BYTE*>(i + 2), pbSig, szMask))
          return reinterpret_cast<BYTE*>(i + 2);

      if ((x & 0xFF000000) >> 24 == bFirstByte)
        if (DataCompare(reinterpret_cast<BYTE*>(i + 3), pbSig, szMask))
          return reinterpret_cast<BYTE*>(i + 3);
    }
    return 0;
  }
}

util::offsets::Signature::Signature(std::string const& sig, int offset /* = 0 */)
{
  Pattern = new BYTE[sig.size()]();
  AddOffset = offset;

  for (size_t i = 0; i < sig.size(); ++i)
  {
    switch (sig[i])
    {
      case ' ':
      {
        break;
      }
      case '[':
      {
        HasReference = true;
        ReferenceOffset = i;
        break;
      }
      case ']':
      {
        ReferenceSize = i - ReferenceOffset;
        break;
      }
      case '?':
      {
        Mask += '?';
        // In signature it's clearer to mark one wildcard byte as ??
        // so skip the next character.
        i += 1;
      }
      default:
      {
        Mask += 'x';
        // Process 2 characters into a single byte
        Pattern[i] = (util::CharToByte(sig[i]) << 4) + util::CharToByte(sig[i+1]);
        i += 1;
      }
    }
  }
}

void util::offsets::Scan()
{
  // Signature example
  // Scan memory and find this pattern. Question marks are wildcard bytes.
  // Brackets mean that the offset is extracted from the assembly reference
  // For example:
  // mov rax,[123456]
  // We want 123456, its place should be marked with wildcard bytes surrounded
  // by brackets in the signature.
  //
  // The last argument is the offset to be added to the result, useful when
  // you need a code offset for byte patches.

  m_Signatures.emplace("OFFSET_EXAMPLE", Signature("12 34 56 78 [ ?? ?? ?? ?? ] AA BB ?? DF", 0x20));

  util::log::Write("Scanning for offsets...");

  MODULEINFO info;
  if (!GetModuleInformation(GetCurrentProcess(), g_gameHandle, &info, sizeof(MODULEINFO)))
  {
    util::log::Error("GetModuleInformation failed, GetLastError 0x%X", GetLastError());
    util::log::Error("Offset scanning unavailable");
    return;
  }

  bool allFound = true;

  for (auto& entry : m_Signatures)
  {
    Signature& sig = entry.second;
    int result = reinterpret_cast<int>(FindPattern((BYTE*)info.lpBaseOfDll, info.SizeOfImage, sig.Pattern, sig.Mask.c_str()));
    if (!result)
    {
      util::log::Error("Could not find pattern for %s", entry.first.c_str());
      allFound = false;
    }

    if (sig.HasReference)
    {
      // Get the assembly reference
      int* pReference = (int*)(result + sig.ReferenceOffset);
      // Assembly reference is relative to the address after the reference
      sig.Result = ((int)pReference + sig.ReferenceSize) + *pReference;
      sig.Result += sig.AddOffset;
    }
    else
      sig.Result = result + sig.AddOffset;
  }

  if (allFound)
    util::log::Ok("All offsets found");
  else
    util::log::Warning("All offsets could not be found, this might result in a crash");

  m_UseScannedResults = true;
}

int util::offsets::GetOffset(std::string const& name)
{
  // If a scan was done, prefer those results.
  // If something couldn't be found or there was no scan,
  // use the hardcoded offsets.

  if (m_UseScannedResults)
  {
    auto result = m_Signatures.find(name);
    if (result != m_Signatures.end())
    {
      if (result->second.Result)
        return result->second.Result;
    }
  }

  // If the offsets were scanned, their absolute position is known.
  // With hardcoded offsets, use relative offset because it's not
  // 100% guaranteed the game module will load at the same address space.

  auto hardcodedResult = m_HardcodedOffsets.find(name);
  if (hardcodedResult != m_HardcodedOffsets.end())
  {
    BuildOffsets const& offsets = hardcodedResult->second;
    switch (m_Build)
    {
      case GameBuild::Steam: return offsets.Steam + (int)g_gameHandle;
      case GameBuild::GOG: return offsets.GOG + (int)g_gameHandle;
      case GameBuild::Epic: return offsets.Epic + (int)g_gameHandle;
    }

    util::log::Error("Offset %s is not known for this release of the game", name.c_str());
    return 0;
  }

  util::log::Error("Offset %s does not exist", name.c_str());
  return 0;
}

util::offsets::GameBuild util::offsets::DetectBuild()
{
  // Each release is told apart by when its exe was linked and how big it is in memory
  const IMAGE_DOS_HEADER* pDosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_gameHandle);
  const IMAGE_NT_HEADERS* pNtHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(g_gameHandle) + pDosHeader->e_lfanew);
  const DWORD timeStamp = pNtHeaders->FileHeader.TimeDateStamp;
  const DWORD imageSize = pNtHeaders->OptionalHeader.SizeOfImage;

  if (timeStamp == 0x54E3199D && imageSize == 0x01F81000)
    m_Build = GameBuild::Steam;
  else if (timeStamp == 0x62952F34 && imageSize == 0x01F9D000)
    m_Build = GameBuild::GOG;
  else if (timeStamp == 0x5FD25676 && imageSize == 0x01F9F000)
    m_Build = GameBuild::Epic;
  else
  {
    m_Build = GameBuild::Unknown;
    util::log::Error("AI.exe linked 0x%X, 0x%X bytes in memory", timeStamp, imageSize);
  }

  return m_Build;
}

const char* util::offsets::BuildName()
{
  switch (m_Build)
  {
    case GameBuild::Steam: return "Steam";
    case GameBuild::GOG: return "GOG";
    case GameBuild::Epic: return "Epic Games Store";
  }
  return "unknown";
}