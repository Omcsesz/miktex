/* internal.h: internal definitions                     -*- C++ -*-

   Copyright (C) 2001-2024 Christian Schenk

   This file is part of MiKTeX Package Manager.

   MiKTeX Package Manager is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2, or
   (at your option) any later version.

   MiKTeX Package Manager is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MiKTeX Package Manager; if not, write to the Free
   Software Foundation, 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA. */

#if !defined(D02AFD831E4F4B9E8394D2B294DF4582)
#define D02AFD831E4F4B9E8394D2B294DF4582

#include <ctime>

#include <string>

#include <miktex/Util/PathName>
#include <miktex/Core/Paths>
#include <miktex/Core/Session>
#include <miktex/Core/VersionNumber>

#include "text.h"

#if !defined(UNUSED)
#  if !defined(NDEBUG)
#    define UNUSED(x)
#  else
#    define UNUSED(x) static_cast<void>(x)
#  endif
#endif

#if !defined(UNUSED_ALWAYS)
#  define UNUSED_ALWAYS(x) static_cast<void>(x)
#endif

#define UNIMPLEMENTED() MIKTEX_INTERNAL_ERROR()

#if defined(MIKTEX_WINDOWS) && 1
#define LOCTRACE() OutputDebugString((std::string(__FILE__) + ":" + std::to_string(__LINE__)).c_str())
#define TRACE(msg) OutputDebugString(msg)
#else
#define LOCTRACE()
#define TRACE(msg)
#endif

/// @namespace MiKTeX::Packages::internal
/// @brief Package manager internals.
MPM_INTERNAL_BEGIN_NAMESPACE;

inline std::string GetMpmAgent()
{
  return "MPM/" + MiKTeX::Core::VersionNumber(MIKTEX_COMPONENT_VERSION_STR).ToString();
}

// the trailing slash should not be removed
constexpr const char* TEXMF_PREFIX_DIRECTORY = "texmf" MIKTEX_PATH_DIRECTORY_DELIMITER_STRING;

struct hash_path
{
public:
  std::size_t operator()(const std::string& str) const
  {
    return MiKTeX::Util::PathName(str).GetHash();
  }
};

struct equal_path
{
public:
  bool operator()(const std::string& str1, const std::string& str2) const
  {
    return MiKTeX::Util::PathName::Equals(MiKTeX::Util::PathName(str1), MiKTeX::Util::PathName(str2));
  }
};

inline void DbgView(const std::string& s)
{
#if defined(_WIN32)
  OutputDebugStringW(UW_("MiKTeX MPM: " + s));
#endif
}

inline bool StripPrefix(const std::string& str, const char* prefix, std::string& result)
{
  std::size_t n = strlen(prefix);
  if (MiKTeX::Util::PathName::ComparePrefixes(MiKTeX::Util::PathName(str), MiKTeX::Util::PathName(prefix), n) != 0)
  {
    return false;
  }
  result = str.c_str() + n;
  return true;
}

#if defined(StrCmp)
#  undef StrCmp
#endif

inline int StrCmp(const char* lpsz1, const char* lpsz2)
{
  return strcmp(lpsz1, lpsz2);
}

inline int StrCmp(const wchar_t* lpsz1, const wchar_t* lpsz2)
{
  return wcscmp(lpsz1, lpsz2);
}

inline bool IsNewer(const MiKTeX::Util::PathName& path1, const MiKTeX::Util::PathName& path2)
{
  return MiKTeX::Core::File::Exists(path1) && MiKTeX::Core::File::Exists(path2) && MiKTeX::Core::File::GetLastWriteTime(path1) > MiKTeX::Core::File::GetLastWriteTime(path2);
}

bool IsUrl(const std::string& url);

std::string MakeUrl(const std::string& base, const std::string& rel);

MPM_INTERNAL_END_NAMESPACE;

#endif
