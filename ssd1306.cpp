//
// ssd1306.cpp
//
// mt32-pi - A bare-metal Roland MT-32 emulator for Raspberry Pi
// Copyright (C) 2020  Dale Whinham <daleyo@gmail.com>
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

#include <algorithm>
#include <cmath>
#include <type_traits>

#include "font6x8.h"
#include "mt32synth.h"
#include "ssd1306.h"

// Compile-time (constexpr) font conversion functions.
// The SSD1306 stores pixel data in columns, but our source font data is stored as rows.
// These templated functions generate column-wise versions of our font at compile-time.
namespace
{
	using CharData = u8[8];

	// Iterate through each row of the character data and collect bits for the nth column
	static constexpr u8 SingleColumn(const CharData& pCharData, u8 pColumn)
	{
		u8 bit = 5 - pColumn;
		u8 column = 0;

		for (u8 i = 0; i < 8; ++i)
			column |= (pCharData[i] >> bit & 1) << i;

		return column;
	}

	// Double the height of the character by duplicating column bits into a 16-bit value
	static constexpr u16 DoubleColumn(const CharData& pCharData, u8 pColumn)
	{
		u8 singleColumn = SingleColumn(pCharData, pColumn);
		u16 column = 0;

		for (u8 i = 0; i < 8; ++i)
		{
			bool bit = singleColumn >> i & 1;
			column |= bit << i * 2 | bit << (i * 2 + 1);
		}

		return column;
	}

	// Templated array-like structure with precomputed font data
	template<size_t N, class F>
	class Font
	{
	public:
		// Result type of conversion function determines array type
		using Column = typename std::result_of<F& (const CharData&, u8)>::type;
		using ColumnData = Column[6];

		constexpr Font(const CharData(&pCharData)[N], F pFunction) : mCharData{ 0 }
		{
			for (size_t i = 0; i < N; ++i)
				for (u8 j = 0; j < 6; ++j)
					mCharData[i][j] = pFunction(pCharData[i], j);
		}

		const ColumnData& operator[](size_t i) const { return mCharData[i]; }

	private:
		ColumnData mCharData[N];
	};

	// Return number of elements in an array
	template<class T, size_t N>
	constexpr size_t ArraySize(const T(&)[N]) { return N; }
}

// Single and double-height versions of the font
constexpr auto FontSingle = Font<ArraySize(Font6x8), decltype(SingleColumn)>(Font6x8, SingleColumn);
constexpr auto FontDouble = Font<ArraySize(Font6x8), decltype(DoubleColumn)>(Font6x8, DoubleColumn);

const u8 CSSD1306::InitSequence[] =
{
	0xAE,       /* Screen off */
	0x81,       /* Set contrast */
		0x7F,   /* 00-FF, default to half */
	
	0xA6,       /* Normal display */

	0x20,       /* Set memory addressing mode */
		0x0,    /* 00 = horizontal */
	0x21,       /* Set column start and end address */
		0x00,
		0x7F,
	0x22,       /* Set page address range */
		0x00,
		0x03,
	
	0xA1,       /* Set segment remap */
	0xA8,       /* Set multiplex ratio */
		0x1F,   /* Screen height - 1 (31) */
	
	0xC8,       /* Set COM output scan direction */
	0xD3,       /* Set display offset */
		0x00,   /* None */
	0xDA,       /* Set com pins hardware configuration */
		0x02,   /* Alternate COM config and disable COM left/right */

	0xD5,       /* Set display oscillator */
		0x80,   /* Default value */
	0xD9,       /* Set precharge period */
		0x22,   /* Default value */
	0xDB,       /* Set VCOMH deselected level */
		0x20,   /* Default */

	0x8D,       /* Set charge pump */
	0x14,       /* VCC generated by internal DC/DC circuit */

	0xA4,       /* Resume to RAM content display */
	0xAF        /* Set display on */
};

CSSD1306::CSSD1306(CI2CMaster *pI2CMaster, u8 pAddress, u8 pHeight)
	: mI2CMaster(pI2CMaster),
	  mAddress(pAddress),
	  mHeight(pHeight),

	  mMessageFlag(false),
	  mPartLevels{0},
	  mPeakLevels{0},
	  mPeakTimes{0},

	  mFramebuffer{0x40}
{
	assert(pHeight == 32 || pHeight == 64);
}

bool CSSD1306::Initialize()
{
	assert(mI2CMaster != nullptr);

	if (!(mHeight == 32 || mHeight == 64))
		return false;

	u8 buffer[] = {0x80, 0};
	for (auto byte : InitSequence)
	{
		buffer[1] = byte;
		mI2CMaster->Write(mAddress, buffer, 2);
	}

	return true;
}

void CSSD1306::WriteFramebuffer() const
{
	// Copy entire framebuffer
	mI2CMaster->Write(mAddress, mFramebuffer, mHeight * 16 + 1);
}

void CSSD1306::SetPixel(u8 pX, u8 pY)
{
	// Ensure range is within 0-127 for x, 0-63 for y
	pX &= 0x7f;
	pY &= 0x3f;

	// The framebuffer starts with the 0x40 byte so that we can write out the entire
	// buffer to the I2C device in one shot, so offset by 1
	mFramebuffer[((pY & 0xf8) << 4) + pX + 1] |= 1 << (pY & 7);
}

void CSSD1306::ClearPixel(u8 pX, u8 pY)
{
	// Ensure range is within 0-127 for x, 0-63 for y
	pX &= 0x7f;
	pY &= 0x3f;

	mFramebuffer[((pY & 0xf8) << 4) + pX + 1] &= ~(1 << (pY & 7));
}

void CSSD1306::DrawChar(char pChar, u8 pCursorX, u8 pCursorY, bool pInverted, bool pDoubleWidth)
{
	size_t rowOffset = pCursorY * 128 * 2;
	size_t columnOffset = pCursorX * (pDoubleWidth ? 12 : 6) + 4;

	for (u8 i = 0; i < 6; ++i)
	{
		u16 fontColumn = FontDouble[static_cast<u8>(pChar - ' ')][i];

		// Don't invert the leftmost column or last two rows
		if (i > 0 && pInverted)
			fontColumn ^= 0x3FFF;

		// Upper half of font
		size_t offset = rowOffset + columnOffset + (pDoubleWidth ? i * 2 : i);

		mFramebuffer[offset] = fontColumn & 0xFF;
		mFramebuffer[offset + 128] = (fontColumn >> 8) & 0xFF;
		if (pDoubleWidth)
		{
			mFramebuffer[offset + 1] = mFramebuffer[offset];
			mFramebuffer[offset + 128 + 1] = mFramebuffer[offset + 128];
		}
	}
}

void CSSD1306::DrawStatusLine(const CMT32SynthBase* pSynth)
{
	// Showing a SysEx message, bail out
	if (mMessageFlag)
		return;

	u32 partStates = pSynth->GetPartStates();
	char buf[9];

	// First 5 parts
	for (u8 i = 0; i < 5; ++i)
	{
		bool state = (partStates >> i) & 1;
		DrawChar(state ? 0x80 : '1' + i, i * 2, 0);
	}

	// Rhythm
	DrawChar((partStates >> 8) ? 0x80 : 'R', 10, 0);

	// Volume
	sprintf(buf, "|vol:%3d", pSynth->GetMasterVolume());
	Print(buf, 12, 0);
}

void CSSD1306::UpdatePartLevels(const CMT32SynthBase* pSynth)
{
	u32 partStates = pSynth->GetPartStates();
	for (u8 i = 0; i < 9; ++i)
	{
		if ((partStates >> i) & 1)
		{
			mPartLevels[i] = floor(VelocityScale * pSynth->GetVelocityForPart(i)) + 0.5f;
			if (mPartLevels[i] > mPeakLevels[i])
			{
				mPeakLevels[i] = mPartLevels[i];
				mPeakTimes[i] = 100;
			}
		}
		else
		{
			if (mPartLevels[i] > 0)
				--mPartLevels[i];
			if (mPeakTimes[i] == 0 && mPeakLevels[i] > 0)
			{
				--mPeakLevels[i];
				mPeakTimes[i] = 3;
			}
			else
				--mPeakTimes[i];
		}
	}
}

void CSSD1306::DrawPartLevels()
{
	for (u8 i = 0; i < 9; ++i)
	{
		// Bar graphs
		u8 topVal, bottomVal;
		if (mPartLevels[i] > 8)
		{
			topVal = 0xFF << (8 - (mPartLevels[i] - 8));
			bottomVal = 0xFF;
		}
		else
		{
			topVal = 0x00;
			bottomVal = 0xFF << (8 - (mPartLevels[i]));
		}

		// Peak meters
		if (mPeakLevels[i] > 8)
			topVal |= 1 << (8 - (mPeakLevels[i] - 8));
		else
			bottomVal |= 1 << (8 - (mPeakLevels[i]));

		for (u8 j = 0; j < 12; ++j)
		{
			mFramebuffer[256 + i * 14 + j + 3] = topVal;
			mFramebuffer[256 + i * 14 + j + 128 + 3] = bottomVal;
		}
	}
}

void CSSD1306::Print(const char* pText, u8 pCursorX, u8 pCursorY, bool pClearLine, bool pImmediate)
{
	while (*pText && pCursorX < 20)
	{
		DrawChar(*pText++, pCursorX, pCursorY);
		++pCursorX;
	}

	if (pClearLine)
	{
		while (pCursorX < 20)
			DrawChar(' ', pCursorX++, pCursorY);
	}

	if (pImmediate)
		WriteFramebuffer();
}

void CSSD1306::Clear()
{
	std::fill(mFramebuffer + 1, mFramebuffer + mHeight * 16 + 1, 0);
	WriteFramebuffer();
}

void CSSD1306::SetMessage(const char* pMessage)
{
	strncpy(mMessageText, pMessage, sizeof(mMessageText));
	mMessageFlag = true;
}

void CSSD1306::ClearMessage()
{
	mMessageFlag = false;
}

void CSSD1306::Update(CMT32SynthBase* pSynth)
{
	if (!pSynth)
		return;

	UpdatePartLevels(pSynth);
	DrawPartLevels();

	if (mMessageFlag)
		Print(mMessageText, 0, 0, true);
	else
		DrawStatusLine(pSynth);

	WriteFramebuffer();
}
