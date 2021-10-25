/* miktex/ptex.h:

   Copyright (C) 2021 Christian Schenk

   This file is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 2, or (at your
   option) any later version.
   
   This file is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this file; if not, write to the Free Software
   Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307,
   USA.  */

#pragma once

#include <kanji.h>

#define frombuff fromBUFF
#define fromdvi fromDVI
#define fromeuc fromEUC
#define fromjis fromJIS
#define fromkuten fromKUTEN
#define fromsjis fromSJIS
#define fromucs fromUCS
#define hi Hi
#define lo Lo
#define tobuff toBUFF
#define todvi toDVI
#define toucs toUCS

template<class FileType> inline void miktexprintencstring(FileType& f)
{
  fprintf(f, " (%s)", get_enc_string());
}