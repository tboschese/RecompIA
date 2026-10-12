/* Execution tracer for snesre, compiled into the snes9x libretro core.
 *
 * Marks every ROM byte the CPU executes, with the M/X/E flags it ran under, and every WRAM byte
 * executed as code (keeping the bytes, since RAM code is copied or generated at runtime). The log
 * is merged into the file named by $SNESRE_TRACE, so several play sessions add up.
 * File layout (little endian):
 *   "SNESRE-TRACE2\0\0\0"  u32 rom_size  u8 rom_flags[rom_size]  u8 wram_flags[0x20000]  u8 wram_code[0x20000]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "snes9x.h"
#include "memmap.h"
#include "snesre_trace.h"

enum
{
	TRACE_OPCODE = 0x01,
	TRACE_OPERAND = 0x02,
	TRACE_M8 = 0x04,
	TRACE_M16 = 0x08,
	TRACE_X8 = 0x10,
	TRACE_X16 = 0x20,
	TRACE_JUMP_TARGET = 0x40,
	TRACE_EMULATION = 0x80,
};

static const char MAGIC[16] = "SNESRE-TRACE2";
static const uint32 WRAM_SIZE = 0x20000;

bool snesre_trace_active = false;
static std::vector<uint8> rom_flags;
static std::vector<uint8> wram_flags;
static std::vector<uint8> wram_code;
static uint32 expected_next = 0xffffffff;

static uint8 *resolve(uint32 address)
{
	uint8 *block = Memory.Map[(address & 0xffffff) >> MEMMAP_SHIFT];
	if ((pint) block < CMemory::MAP_LAST)
		return NULL;
	return block + (address & 0xffff);
}

void snesre_trace_start(void)
{
	snesre_trace_active = getenv("SNESRE_TRACE") != NULL;
	rom_flags.assign(Memory.CalculatedSize, 0);
	wram_flags.assign(WRAM_SIZE, 0);
	wram_code.assign(WRAM_SIZE, 0);
	expected_next = 0xffffffff;
}

void snesre_trace(uint32 pbpc, uint16 p, uint8 length)
{
	uint8 flags = (p & 0x100) ? TRACE_EMULATION | TRACE_M8 | TRACE_X8
		: ((p & 0x20) ? TRACE_M8 : TRACE_M16) | ((p & 0x10) ? TRACE_X8 : TRACE_X16);
	if (pbpc != expected_next)
		flags |= TRACE_JUMP_TARGET;
	expected_next = (pbpc & 0xff0000) | ((pbpc + length) & 0xffff);

	uint8 *byte = resolve(pbpc);
	if (byte >= Memory.ROM && byte < Memory.ROM + rom_flags.size())
	{
		size_t offset = byte - Memory.ROM;
		rom_flags[offset] |= TRACE_OPCODE | flags;
		for (uint8 i = 1; i < length && offset + i < rom_flags.size(); i++)
			rom_flags[offset + i] |= TRACE_OPERAND;
	}
	else if (byte >= Memory.RAM && byte < Memory.RAM + WRAM_SIZE)
	{
		size_t offset = byte - Memory.RAM;
		wram_flags[offset] |= TRACE_OPCODE | flags;
		for (uint8 i = 0; i < length && offset + i < WRAM_SIZE; i++)
		{
			if (i)
				wram_flags[offset + i] |= TRACE_OPERAND;
			wram_code[offset + i] = byte[i];
		}
	}
}

void snesre_trace_save(void)
{
	const char *path = getenv("SNESRE_TRACE");
	if (!snesre_trace_active || !path || rom_flags.empty())
		return;

	std::vector<uint8> rom(rom_flags), wram(wram_flags), code(wram_code);
	FILE *in = fopen(path, "rb");
	if (in)
	{
		char magic[16];
		uint32 size = 0;
		if (fread(magic, 1, 16, in) == 16 && !memcmp(magic, MAGIC, 16) && fread(&size, 4, 1, in) == 1 && size == rom.size())
		{
			std::vector<uint8> old(size + 2 * WRAM_SIZE);
			if (fread(old.data(), 1, old.size(), in) == old.size())
			{
				for (uint32 i = 0; i < size; i++)
					rom[i] |= old[i];
				for (uint32 i = 0; i < WRAM_SIZE; i++)
				{
					if (!wram[i])
						code[i] = old[size + WRAM_SIZE + i];
					wram[i] |= old[size + i];
				}
			}
		}
		fclose(in);
	}

	std::string tmp = std::string(path) + ".part";
	FILE *out = fopen(tmp.c_str(), "wb");
	if (!out)
		return;
	uint32 size = rom.size();
	fwrite(MAGIC, 1, 16, out);
	fwrite(&size, 4, 1, out);
	fwrite(rom.data(), 1, rom.size(), out);
	fwrite(wram.data(), 1, wram.size(), out);
	fwrite(code.data(), 1, code.size(), out);
	fclose(out);
	rename(tmp.c_str(), path);
}
