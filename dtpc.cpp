// license:BSD-3-Clause
// copyright-holders:Carl, akse0435
// dtpc.cpp - standalone MAME machine for the DECtalk PC.
//
// ACCESSIBILITY: everything is logged to the console AND to "dtpc.log" in the working dir.
// Each step of boot and module download is narrated, so a screen-reader user can
// follow along without reading anything graphical in the MAME window.
//
// PROTOCOL (recovered from the original PC's DT_LOAD.EXE + DT_DRIV.EXE, cross-checked):
//   Status: 16-bit, read at base+0(low)/base+1(high). Wait for bit 0x10 (dma_ready).
//   DMA channel (base+4): prefix 5 (CONTROL), then sub-code:
//     LOAD_MEM=6, START_TASK=5, MEM_ALLOC=3, SET_DIC=4
//   Kernel: 5,6, 00,04,00,00, count, data...  then  5,5,[wait0x10], ip,cs.
//   Single byte (runtime): single_in=1, single_out=2.
//
// Kernel bootstrap is fully recovered and runs by default (the verifiable
// test). The other modules (ALLOC/LOAD/START + dictionary) are loaded afterwards
// and work. The log shows the progress along the way.

#include "emu.h"
#include "cpu/i86/i186.h"
#include "cpu/tms320c1x/tms320c1x.h"
#include "sound/dac.h"
#include "speaker.h"
#include "screen.h"
#include <chrono>

#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <algorithm>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <termios.h>
  #include <dirent.h>
  #if defined(__APPLE__)
    #include <util.h>
  #else
    #include <pty.h>
  #endif
#endif

namespace {

// Global quiet mode: logging is OFF by default. Set the
// environment variable DTPC_LOG=1 to turn ALL non-critical logging ON (both
// file and console) - useful for debugging. Without DTPC_LOG (or with
// DTPC_LOG=0) the emulator runs silently, which also gives pure
// emulation speed without disk/console overhead (production mode).
static bool g_quiet = true;

class accessible_log {
public:
	void open(const char *path) {
		g_quiet = true;                      // default: no logging
		if (const char *e = getenv("DTPC_LOG")) g_quiet = (atoi(e) == 0); // log only when DTPC_LOG != 0
		if (g_quiet) return;                 // no log file when logging is off
		m_f.open(path, std::ios::app); line("===== dtpc-session start =====");
	}
	void line(const char *fmt, ...) {
		if (g_quiet) return;                 // skip everything - no formatting, no I/O
		char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
		osd_printf_info("[dtpc] %s\n", b);
		if (m_f) {
			m_f << b << "\n";
			if (++m_since_flush >= 64) { m_f.flush(); m_since_flush = 0; }
		}
	}
	void flush_now() { if (m_f) { m_f.flush(); m_since_flush = 0; } }
private:
	std::ofstream m_f;
	int m_since_flush = 0;
};

class serial_bridge {
public:
	~serial_bridge() { close(); }
	bool open(const std::string &name, int baud) {
#ifdef _WIN32
		HANDLE h = CreateFileA(name.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			m_handle = nullptr;
			return false;
		}
		DCB dcb;
		memset(&dcb, 0, sizeof(dcb));
		dcb.DCBlength = sizeof(dcb);
		GetCommState(h, &dcb);
		dcb.BaudRate = baud;
		dcb.ByteSize = 8;
		dcb.Parity = NOPARITY;
		dcb.StopBits = ONESTOPBIT;
		dcb.fBinary = TRUE;
		SetCommState(h, &dcb);
		COMMTIMEOUTS to;
		memset(&to, 0, sizeof(to));
		to.ReadIntervalTimeout = MAXDWORD;
		SetCommTimeouts(h, &to);
		m_handle = h;
		return true;
#else
		if (name.empty()) {
			int mfd = -1, sfd = -1;
			char sn[256] = {0};
			if (openpty(&mfd, &sfd, sn, nullptr, nullptr) != 0)
				return false;
			m_fd = mfd;
			m_pty = sn;
			fcntl(m_fd, F_SETFL, O_NONBLOCK);
			return true;
		}
		m_fd = ::open(name.c_str(), O_RDWR|O_NOCTTY|O_NONBLOCK);
		if (m_fd < 0)
			return false;
		termios t;
		tcgetattr(m_fd, &t);
		cfmakeraw(&t);
		(void)baud;
		tcsetattr(m_fd, TCSANOW, &t);
		return true;
#endif
	}
	void close() {
#ifdef _WIN32
		if (m_handle) {
			CloseHandle((HANDLE)m_handle);
			m_handle = nullptr;
		}
#else
		if (m_fd >= 0) {
			::close(m_fd);
			m_fd = -1;
		}
#endif
	}
	int read_available(uint8_t *b, int mx) {
#ifdef _WIN32
		if (!m_handle)
			return 0;
		DWORD g = 0;
		if (!ReadFile((HANDLE)m_handle, b, (DWORD)mx, &g, nullptr))
			return 0;
		return (int)g;
#else
		if (m_fd < 0)
			return 0;
		ssize_t n = ::read(m_fd, b, (size_t)mx);
		return n > 0 ? (int)n : 0;
#endif
	}
	void write_bytes(const uint8_t *b, int n) {
#ifdef _WIN32
		if (!m_handle)
			return;
		DWORD w = 0;
		WriteFile((HANDLE)m_handle, b, (DWORD)n, &w, nullptr);
#else
		if (m_fd < 0)
			return;
		ssize_t r = ::write(m_fd, b, (size_t)n);
		(void)r;
#endif
	}
	const std::string &pty_name() const { return m_pty; }
private:
#ifdef _WIN32
	void *m_handle=nullptr;
#else
	int m_fd=-1;
#endif
	std::string m_pty;
};

// Recovered opcodes / bits (from the DT_DRIV handlers, verified against the jump table).
enum : uint8_t { DMA_SINGLE_IN=0x01, DMA_SINGLE_OUT=0x02,
                 CTRL=0x05, OP_LOAD_MEM=0x06, OP_START=0x05, OP_ALLOC=0x03, OP_SETDIC=0x04 };
enum : uint16_t { STAT_TR_CHAR=0x0002,   // card has a character ready (GET_CHAR tests bit 2)
                  STAT_RR_CHAR=0x0004,   // card can receive a character
                  STAT_CMD_ACK=0x0008,   // kernel has accepted command (after 0x6000)
                  STAT_DMA_READY=0x0010,
                  STAT_FLOP=0x0100 };    // DMA flip-flop: toggles at each handshake step
// Init handshake (command register), recovered from DT_LOAD.EXE:
//   card-ready = 0x0DEC; we send 0x5000/0x6000/0x5000 and wait for 0x8000/0xF000/0x8000.
enum : uint16_t { ST_READY=0x0DEC, CMD_GO=0x0000, CMD_A=0x5000, CMD_B=0x6000,
                  CMD_LOAD=0x4000, CMD_START=0x4001, ST_ACK1=0x8000, ST_ACK2=0xF000 };

// A loaded module + parsed MZ.
struct module_t {
	std::string name;
	std::vector<uint8_t> file;       // raw file
	std::vector<uint8_t> image;      // relocatable image (without header)
	std::vector<std::pair<uint16_t,uint16_t>> relocs;
	uint16_t ip=0, cs=0, minalloc=0; bool is_mz=false;
};

class dtpc_state : public driver_device {
public:
	dtpc_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig,type,tag), m_cpu(*this,"maincpu"), m_dsp(*this,"dsp"), m_dac(*this,"dac") {}
	void dtpc(machine_config &config);
	uint32_t screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect) { return 0; }
protected:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;
private:
	void status_w(uint16_t data);
	uint16_t cmd_r() { return m_cmd; }
	void data_w(uint16_t data) { m_data=data; }
	uint16_t data_r() { return m_data; }
	uint16_t host_irq_r() { return 0; }
	uint8_t dma_r() { m_latch_reads++; m_latch_fresh=false; return m_dma; }
	void dma_w(uint8_t data) {
		// If the card has not yet read the previous byte (latch still "fresh"),
		// we count a collision: a host byte overwrote an unread byte.
		if (m_latch_fresh) m_latch_collisions++;
		m_dma=data; m_latch_fresh=true; m_latch_writes++;
	}
	void dac_w(uint16_t data) { m_dac_writes++; m_dac->write(data>>4); }
	void output_ctl_w(uint16_t data);
	uint16_t dsp_dma_r();
	void dsp_dma_w(uint16_t data);
	int bio_line_r();
	void irq_line_w(uint16_t) { m_cpu->int1_w(0); }
	void clock_w(int state);

	void host_w(offs_t off, uint8_t data);
	uint8_t host_r(offs_t off, bool se=true);

	void cpu_map(address_map &map) ATTR_COLD;
	void cpu_io(address_map &map) ATTR_COLD;
	void dsp_map(address_map &map) ATTR_COLD;
	void dsp_io(address_map &map) ATTR_COLD;

	TIMER_CALLBACK_MEMBER(pump_tick);
	uint16_t hp_status() { return (uint16_t(host_r(1,false))<<8)|host_r(0,false); }
	void dput(uint8_t b) { host_w(4,b); }            // write one byte to the DMA channel
	void put_command(uint16_t c) { host_w(0, c & 0xff); host_w(1, c >> 8); } // write command register
	void load_module_files();
	bool parse_mz(module_t &m);
	void relocate(module_t &m, uint16_t segpara, std::vector<uint8_t> &out);
	void download_tick();                            // state machine

	required_device<i80186_cpu_device> m_cpu;
	required_device<tms320c15_device> m_dsp;
	required_device<dac_12bit_r2r_twos_complement_device> m_dac;

	uint16_t m_cmd=0,m_stat=0,m_data=0,m_dsp_dma=0,m_ctl=0;
	uint8_t m_dma=0,m_vol=63,m_bio=ASSERT_LINE;

	accessible_log m_log;
	serial_bridge m_serial;
	emu_timer *m_pump=nullptr;
	uint16_t m_last_stat=0xffff;

	// download state
	int m_phase=0;
	int m_dl=0;
	std::vector<module_t> m_mods;
	std::vector<uint8_t> m_cur;                     // current relocated image being sent
	std::vector<uint8_t> m_tx; size_t m_txoff=0;    // byte stream to base+4 (LOAD_MEM)
	int m_settle=0;
	long m_stat_writes=0;                           // number of status writes (CPU liveness)
	int m_wd_ticks=0; int m_wd_sig=-1;              // watchdog: detects lockups

	// ---- action engine for module download in kernel mode ----
	// Each step is separated by WaitDstat: wait for status bit 0x0100 to TOGGLE.
	struct action { int type; std::vector<uint8_t> bytes; const char *note; };
	enum { A_WAIT=0, A_SEND=1, A_READ4=2, A_NOTE=3, A_READ1=4, A_VERIFY=5, A_SENDRAW=6,
	       // ---- runtime command path (abort/flush), recovered from 4.2 DT_DRIV ----
	       // do_immd pattern: wait_cstat -> put_data -> put_command(+doorbell).
	       A_WAIT_DSTAT=7,  // wait for flop bit (0x0100) to toggle   (= wait_dstat)
	       A_WAIT_CSTAT=8,  // wait for status bit 0x0008 SET        (= wait_cstat)
	       A_PUTDATA=9,     // write data register base+2/+3         (= put_data)
	       A_PUTCMD=10,     // write command base+0/+1 + doorbell base+6, wait for 0x0008 CLEARED (= put_command)
	       A_WAIT_FLUSH=11, // wait for status bit 0x0400 CLEARED     (= wait_flush)
	       A_ACK=12 };      // send ^A (0x01) back to JAWS as a flush acknowledgement
	std::vector<action> m_acts; size_t m_aidx=0, m_aoff=0;
	uint8_t m_rd4[4] = {0,0,0,0};
	uint16_t m_flop=0;                              // last value of bit 0x0100
	int m_flop_skips=0;                             // number of times we continued without a toggle
	uint32_t m_alloc_lin=0;                         // linear address from ALLOC
	size_t m_ordidx=0; int m_xstage=0;              // module order + stage
	std::vector<uint8_t> m_rtq; int m_rt_mode=0;    // runtime text pipe (1=send, 2=fetch)
	bool m_rt_first_tx=false, m_rt_first_rx=false, m_rt_first_in=false;
	long m_rt_tx_count=0, m_rt_rx_count=0;
	long m_rt_idle=0;
	unsigned m_dsp_mhz=80;
	double m_out_gain=1.0;   // extra output gain after the card (DTPC_GAIN; 1.0=unchanged)
	std::chrono::steady_clock::time_point m_wall0;
	long m_dac_at_mark=0; bool m_wall_init=false;
	double m_emu_at_mark=0;
	std::map<uint32_t,int> m_pch;                   // PC profile during a block
	bool m_dsp_running=false; long m_ctl_writes=0;
	long m_bio_polls=0;      // DSP polls BIO (= the DSP's ROM loop is running)
	long m_dsp_feed=0;       // words fed to the DSP (cpu/DMA0 -> port 0x500)
	long m_dsp_consume=0;    // words consumed by the DSP (DSP reads port 0)
	long m_dac_writes=0;     // samples out to the DAC
	int  m_ctl_log=0;        // rate-limit on CTL log lines
	long m_stall_ticks=0;    // times A_SEND waited for DMA1 to be armed
	long m_latch_writes=0, m_latch_reads=0, m_latch_collisions=0;
	bool m_latch_fresh=false; // true = host has written a byte the card has not read yet
	module_t *find_mod(const char *name);
	void addr4(uint32_t lin, std::vector<uint8_t> &out);
	void build_alloc_actions(module_t &m);
	void build_xfer_actions(module_t &m, bool is_dict);
	void build_flush_actions();                     // abort/flush (^C from JAWS)
	bool run_actions();                             // true = list finished

	// ---- abort/flush state ----
	bool m_flush_pending=false;                     // set when ^C (0x03) is seen in the serial stream
	long m_flush_count=0;                           // number of flushes performed (diagnostics)
	// ---- flood guard on GET_CHAR ----
	uint8_t m_get_last=0;                           // last byte fetched from the card
	long m_get_run=0;                               // number of identical bytes in a row
	bool m_get_flood=false;                         // true = card is flooding; stop forwarding to JAWS
	// ---- runtime check_dstat + clean abort (faithful to the real driver) ----
	// dstat_ready() == the driver's check_dstat (DT_DRIV sub_11EEE): has the flop bit (0x0100)
	// ALREADY toggled since the last consumed toggle (m_flop)? We never start a
	// SEND/GET handshake without a genuinely available toggle, and therefore never
	// write an opcode to base+4 out of turn.
	bool dstat_ready() { return (uint16_t)(hp_status() & 0x0100) != m_flop; }
	int  m_rt_wait_limit=4;       // bounded runtime timeout in 12000-tick cycles (4 ~= 240 ms)
	int  m_rt_get_wait_max=200;   // hard cap for waiting for the card to process a GET (last resort)
	bool m_rt_abort=false;        // set when a bounded runtime wait times out
	uint8_t m_rt_pending_char=0;  // the character a SEND is about to send (for re-queue on abort)
	bool m_rt_pending_valid=false;
	// After a flush the card's output ring is empty, but TR_char may linger as
	// "stale" (the kernel's single_out only recomputes TR/RR when a character is
	// ACTUALLY delivered, sub_13C48 - on an empty ring it is skipped and the card echoes the stale
	// opcode byte 0x02 from the shared buffer). Draining the bridge on the stale TR_char
	// yields a self-feeding 0x02 flood. We therefore block autonomous GET draining
	// from when a flush finishes until the next real SEND (only then can the card have new output).
	bool m_drain_after_flush_blocked=false;
};

void dtpc_state::status_w(uint16_t data) {
	m_stat_writes++;
	// During boot/download (phase 0-1) the status log is valuable diagnostics.
	// During runtime (phase 2) status changes hundreds of times per second
	// (flop toggling during speech) - it would flood the log and block
	// emulation with disk I/O. So we do not log it there.
	if (data != m_last_stat) {
		if (m_phase < 2) m_log.line("Card status -> 0x%04X", data);
		m_last_stat=data;
	}
	m_stat=data;
}
void dtpc_state::output_ctl_w(uint16_t data) {
	if (!(data&8)&&!(m_ctl&2)&&(data&2)) {
		if ((data&4)&&(m_vol<64))
			m_vol++;
		else if (!(data&4)&&m_vol)
			m_vol--;
		m_dac->set_output_gain(ALL_OUTPUTS, m_vol/63.0);
	}
	bool run = (data & 0x10) != 0;
	if (run != m_dsp_running) {
		m_ctl_log++;
		if (m_ctl_log <= 12 || (m_ctl_log & 0x3ff) == 0)
			m_log.line("CTL: DSP %s (ctl=0x%04X, change no. %d, ctl write no. %ld).",
				run ? "STARTED" : "stopped (reset)", data, m_ctl_log, m_ctl_writes+1);
		if (m_ctl_log == 12)
			m_log.line("(throttling CTL log - showing only every 1024th change)");
		m_dsp_running = run;
	}
	m_ctl_writes++;
	m_dsp->set_input_line(INPUT_LINE_RESET, (data&0x10)?CLEAR_LINE:ASSERT_LINE);
	m_ctl=data;
}
uint16_t dtpc_state::dsp_dma_r() { m_dsp_consume++; m_bio=ASSERT_LINE; return m_dsp_dma; }
void dtpc_state::dsp_dma_w(uint16_t data) { m_dsp_feed++; m_bio=CLEAR_LINE; m_dsp_dma=data; }
int dtpc_state::bio_line_r() {
	m_bio_polls++;
	if (m_bio==ASSERT_LINE)
		m_cpu->dma_sync_req(0);
	return m_bio;
}
void dtpc_state::clock_w(int state) { m_dsp->set_input_line(INPUT_LINE_IRQ0, (!(m_ctl&0x20)||state)?CLEAR_LINE:ASSERT_LINE); }
void dtpc_state::host_w(offs_t off, uint8_t data) {
	switch (off) {
		case 0: m_cmd=(m_cmd&0xff00)|data; break;
		case 1: m_cmd=(m_cmd&0x00ff)|(data<<8); break;
		case 2: m_data=(m_data&0xff00)|data; break;
		case 3: m_data=(m_data&0x00ff)|(data<<8); break;
		case 4: m_dma=data; m_cpu->dma_sync_req(1); break;
		case 6: m_cpu->int1_w(1); break;
	}
}
uint8_t dtpc_state::host_r(offs_t off, bool se) {
	switch (off) {
		case 0: return m_stat&0xff;
		case 1: return m_stat>>8;
		case 2: return m_data&0xff;
		case 3: return m_data>>8;
		case 4:
			if (se)
				m_cpu->dma_sync_req(1);
			return m_dma;
	}
	return 0;
}
void dtpc_state::cpu_io(address_map &map) {
	map(0x0400,0x0401).rw(FUNC(dtpc_state::cmd_r),FUNC(dtpc_state::status_w));
	map(0x0480,0x0481).rw(FUNC(dtpc_state::data_r),FUNC(dtpc_state::data_w));
	map(0x0500,0x0501).w(FUNC(dtpc_state::dsp_dma_w));
	map(0x0580,0x0581).r(FUNC(dtpc_state::host_irq_r));
	map(0x0600,0x0601).w(FUNC(dtpc_state::output_ctl_w));
	map(0x0680,0x0680).rw(FUNC(dtpc_state::dma_r),FUNC(dtpc_state::dma_w));
	map(0x0700,0x0701).w(FUNC(dtpc_state::irq_line_w));
}
void dtpc_state::cpu_map(address_map &map) { map(0x00000,0xfbfff).ram(); map(0xfc000,0xfffff).rom().region("maincpu",0); }
void dtpc_state::dsp_io(address_map &map) {
	map(0x0,0x0).r(FUNC(dtpc_state::dsp_dma_r));
	map(0x1,0x1).rw(FUNC(dtpc_state::dsp_dma_r),FUNC(dtpc_state::dac_w));
}
void dtpc_state::dsp_map(address_map &map) { map(0x0000,0x0fff).rom().region("dsp",0); }

// ---------- MZ parse + relocation ----------
bool dtpc_state::parse_mz(module_t &m) {
	auto &f=m.file; if (f.size()<0x20||f[0]!='M'||f[1]!='Z') return false;
	auto r16=[&](size_t o){ return uint16_t(f[o]|(f[o+1]<<8)); };
	uint16_t cblp=r16(2),cp=r16(4),crlc=r16(6),cparhdr=r16(8),lfarlc=r16(0x18);
	m.ip=r16(0x14); m.cs=r16(0x16); m.minalloc=r16(0x0a);
	size_t hdr=size_t(cparhdr)*16;
	size_t end=(cblp==0)?size_t(cp)*512:size_t(cp-1)*512+cblp; if(end>f.size())end=f.size();
	if (hdr>end) return false;
	m.image.assign(f.begin()+hdr,f.begin()+end);
	for (uint16_t i=0;i<crlc;i++){ size_t o=lfarlc+i*4; if(o+4>f.size())break; m.relocs.emplace_back(r16(o),r16(o+2)); }
	m.is_mz=true; return true;
}
void dtpc_state::relocate(module_t &m, uint16_t seg, std::vector<uint8_t> &out) {
	out=m.image;
	for (auto &rl:m.relocs){ uint32_t a=uint32_t(rl.second)*16+rl.first; if(a+1<out.size()){
		uint16_t v=out[a]|(out[a+1]<<8); v+=seg; out[a]=v&0xff; out[a+1]=v>>8; } }
}

void dtpc_state::load_module_files() {
	const char *mods=getenv("DTPC_MODULES");
	// Default: the "modules" subfolder in the working dir, so you need not
	// set DTPC_MODULES. Set the variable to point somewhere else.
	std::string dir = (mods && *mods) ? mods : "modules";
	if (!mods || !*mods)
		m_log.line("DTPC_MODULES not set - using the default folder \"modules\".");

	// Clean the path: strip any surrounding quotes and trailing separators.
	if (dir.size()>=2 && dir.front()=='"' && dir.back()=='"') dir = dir.substr(1, dir.size()-2);
	while (!dir.empty() && (dir.back()=='/' || dir.back()=='\\' || dir.back()==' ')) dir.pop_back();
	m_log.line("Module folder (DTPC_MODULES): \"%s\"", dir.c_str());

	// Show what is ACTUALLY in the folder - reveals wrong names/subfolders.
	m_log.line("Folder contents:");
	bool any=false;
#ifdef _WIN32
	WIN32_FIND_DATAA fd; std::string pat = dir + "\\*";
	HANDLE h = FindFirstFileA(pat.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		m_log.line("  *** Could NOT open the folder. Does \"%s\" exist? Is the path correct?", dir.c_str());
	} else {
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				if (strcmp(fd.cFileName,".") && strcmp(fd.cFileName,".."))
					{ m_log.line("    [subfolder] %s", fd.cFileName); any=true; }
			} else {
				m_log.line("    %s (%lu bytes)", fd.cFileName, (unsigned long)fd.nFileSizeLow); any=true;
			}
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
#else
	DIR *dp = opendir(dir.c_str());
	if (!dp) {
		m_log.line("  *** Could NOT open the folder \"%s\".", dir.c_str());
	} else {
		struct dirent *de;
		while ((de = readdir(dp)) != nullptr) {
			if (strcmp(de->d_name,".") && strcmp(de->d_name,"..")) { m_log.line("    %s", de->d_name); any=true; }
		}
		closedir(dp);
	}
#endif
	if (!any) m_log.line("  (folder is empty or inaccessible)");

	// Try to open each expected module; log the EXACT path tried.
	const char *names[]={"kernel.sys","dtpcdic.dic","lts.exe","ph.exe","cmd.exe","usa.exe","dtpc_850.exe"};
#ifdef _WIN32
	const char *seps = "\\/";
#else
	const char *seps = "/";
#endif
	for (const char *nm : names) {
		module_t m; m.name = nm;
		std::string used; bool ok=false;
		for (const char *sp = seps; *sp && !ok; ++sp) {
			std::string p = dir; p += *sp; p += nm;
			std::ifstream f(p, std::ios::binary);
			if (f) {
				m.file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
				used = p; ok = true;
			}
		}
		// Dictionary fallback: if dtpcdic.dic is missing, dtpc.dic is tried instead.
		// The module name stays "dtpcdic.dic", so the rest of the code is unchanged.
		if (!ok && std::string(nm) == "dtpcdic.dic") {
			for (const char *sp = seps; *sp && !ok; ++sp) {
				std::string p = dir; p += *sp; p += "dtpc.dic";
				std::ifstream f(p, std::ios::binary);
				if (f) {
					m.file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
					used = p; ok = true;
				}
			}
		}
		if (!ok) {
			m_log.line("  MISSING: %s (tried path: \"%s%c%s\")", nm, dir.c_str(), seps[0], nm);
			continue;
		}
		if (std::string(nm).find(".dic") == std::string::npos) {
			if (!parse_mz(m)) m_log.line("  WARNING: %s is not a valid MZ", nm);
		}
		m_log.line("  found: %-13s %zu bytes%s  [%s]", nm, m.file.size(), m.is_mz?" (MZ)":"", used.c_str());
		m_mods.push_back(std::move(m));
	}
}

// ---------- action engine (kernel mode) ----------
module_t *dtpc_state::find_mod(const char *name) {
	for (auto &m : m_mods)
		if (m.name == name)
			return &m;
	return nullptr;
}
// The address in the LOAD_MEM header is encoded as 32-bit linear, low word first.
//   For 0x8D180 -> word0=0xD180, word1=0x0008 -> expected DST = 0x8D180.
void dtpc_state::addr4(uint32_t lin, std::vector<uint8_t> &out) {
	uint16_t lo = (uint16_t)(lin & 0xffff);
	uint16_t hi = (uint16_t)((lin >> 16) & 0xffff);
	out.push_back(lo & 0xff); out.push_back(lo >> 8);
	out.push_back(hi & 0xff); out.push_back(hi >> 8);
}
void dtpc_state::build_alloc_actions(module_t &m) {
	uint16_t paras;
	if (m.is_mz) {
		// Exactly as DT_LOAD's source (ldr_file.c load_exe):
		//   total_paras = (image_left>>4) + hmin + 16
		// Note: >>4 rounds DOWN (not up). hmin = minalloc (BSS). +16 paras slack.
		paras = (uint16_t)((m.image.size() >> 4) + m.minalloc + 16);
		m_log.line("ALLOC size: %zu image bytes (>>4=%zu paras) + min_alloc %u + 16 = %u paras.",
			m.image.size(), m.image.size()>>4, m.minalloc, paras);
	} else {
		// Dictionary: exactly as the source's load_dic:
		//   total_paras = ((entries*4)+dic_bytes)>>4)+2
		uint32_t entries = (uint32_t)m.file[0]|((uint32_t)m.file[1]<<8)|((uint32_t)m.file[2]<<16)|((uint32_t)m.file[3]<<24);
		uint32_t dsize   = (uint32_t)m.file[4]|((uint32_t)m.file[5]<<8)|((uint32_t)m.file[6]<<16)|((uint32_t)m.file[7]<<24);
		paras = (uint16_t)(((entries*4 + dsize) >> 4) + 2);
		m_log.line("Dictionary: %u entries (index %u bytes) + %u data bytes = %u paras.",
			entries, entries*4, dsize, paras);
	}
	m_acts.clear(); m_aidx = 0; m_aoff = 0;
	m_acts.push_back({A_NOTE, {}, "ALLOC: waiting for flop toggle..."});
	m_acts.push_back({A_WAIT, {}, nullptr});
	m_acts.push_back({A_SEND, {CTRL, OP_ALLOC}, nullptr});
	m_acts.push_back({A_WAIT, {}, nullptr});
	m_acts.push_back({A_SEND, {(uint8_t)(paras & 0xff), (uint8_t)(paras >> 8)}, nullptr});
	m_acts.push_back({A_WAIT, {}, nullptr});
	m_acts.push_back({A_READ4, {}, nullptr});
}
void dtpc_state::build_xfer_actions(module_t &m, bool is_dict) {
	uint16_t para = (uint16_t)(m_alloc_lin >> 4);
	const std::vector<uint8_t> *src;
	if (m.is_mz) { relocate(m, para, m_cur); src = &m_cur; }
	else {
		// Dictionary: 8-byte header removed; the index table (entries*4 bytes)
		// is relocated by adding the load paragraph to the SEGMENT word in each
		// entry (exactly as DT_LOAD does), then raw data.
		uint32_t entries = (uint32_t)m.file[0]|((uint32_t)m.file[1]<<8)|((uint32_t)m.file[2]<<16)|((uint32_t)m.file[3]<<24);
		size_t isz = (size_t)entries*4;
		m_cur.assign(m.file.begin()+8, m.file.end());
		for (size_t e=0; e<isz && e+3<m_cur.size(); e+=4) {
			uint16_t seg = m_cur[e+2] | (m_cur[e+3]<<8);
			seg += para;
			m_cur[e+2]=seg&0xff; m_cur[e+3]=seg>>8;
		}
		m_log.line("Dictionary index relocated (+0x%04X on %u entries).", para, entries);
		src = &m_cur;
	}

	m_acts.clear(); m_aidx = 0; m_aoff = 0;
	// LOAD_MEM in chunks (16-bit count per transaction)
	const uint32_t CHUNK = 0x4000;
	uint32_t total = (uint32_t)src->size(), off = 0;
	while (off < total) {
		uint32_t n = (total - off > CHUNK) ? CHUNK : (total - off);
		std::vector<uint8_t> hdr;
		addr4(m_alloc_lin + off, hdr);
		hdr.push_back(n & 0xff); hdr.push_back((n >> 8) & 0xff);
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_SEND, {CTRL, OP_LOAD_MEM}, nullptr});
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_SEND, hdr, nullptr});
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_SEND, std::vector<uint8_t>(src->begin()+off, src->begin()+off+n), "  ...chunk sent"});
		off += n;
	}
	m_acts.push_back({A_VERIFY, {}, nullptr});
	if (is_dict) {
		// SET_DIC: 5,4 + 10-byte block: address(4) + entry count(4) + type(2).
		// The entry count is read from the dictionary file's first dword.
		uint32_t entries = (uint32_t)m.file[0] | ((uint32_t)m.file[1]<<8)
		                 | ((uint32_t)m.file[2]<<16) | ((uint32_t)m.file[3]<<24);
		// SET_DIC (INT2F D0h/10h) uses a different address format than LOAD_MEM.
		// DT_LOAD's set_dic wrapper (sub_115F0) normalizes the address to a
		// FAR pointer before sending:
		//   word0 = address & 0x0F   (offset within the paragraph)
		//   word1 = address >> 4     (segment/paragraph)
		// So addr4()'s linear format is not used here.
		uint16_t dic_off = (uint16_t)(m_alloc_lin & 0x0F);
		uint16_t dic_seg = (uint16_t)(m_alloc_lin >> 4);
		std::vector<uint8_t> blk;
		blk.push_back(dic_off & 0xff); blk.push_back(dic_off >> 8);
		blk.push_back(dic_seg & 0xff); blk.push_back(dic_seg >> 8);
		blk.push_back(entries & 0xff); blk.push_back((entries>>8) & 0xff);
		blk.push_back((entries>>16) & 0xff); blk.push_back((entries>>24) & 0xff);
		// Type word: primary ENGLISH = 0. Confirmed in our 4.2CD DT_LOAD.EXE:
		// load_dic (sub_11246) is called for the English dictionary at loc_10330 with
		// 'push 0', while 'push 4' is the SPANISH call (loc_104BC).
		const uint16_t DICT_TYPE_PRIMARY_ENGLISH = 0;
		blk.push_back(DICT_TYPE_PRIMARY_ENGLISH & 0xff); blk.push_back(DICT_TYPE_PRIMARY_ENGLISH >> 8);
		m_log.line("SET_DIC: off 0x%04X, seg 0x%04X, %u entries, type %u (primary English).",
			dic_off, dic_seg, entries, DICT_TYPE_PRIMARY_ENGLISH);
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_SEND, {CTRL, OP_SETDIC}, nullptr});
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_SEND, blk, "SET_DIC block sent"});
	} else {
		// START_TASK: 5,5 + entry (ip, cs+para) - like the ROM path
		uint16_t cs = m.cs + para, ip = m.ip;
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_SEND, {CTRL, OP_START}, "START_TASK being sent"});
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_SEND, {(uint8_t)(ip&0xff),(uint8_t)(ip>>8),(uint8_t)(cs&0xff),(uint8_t)(cs>>8)}, nullptr});
	}
}
// ---------- abort/flush (^C from JAWS) ----------
// Builds the action list that matches the original 4.2 DT_DRIV's
// FLUSH_TEXT (multiplex function 0x17) EXACTLY, verified from the DT_DRIV.EXE disassembly:
//   wait_dstat(30)                         ; wait for flop toggle (0x0100)
//   do_immd(cmd=0x1600, data=0)            ; CMD_control(0x1000)+CTRL_flush(0x600)
//       = wait_cstat(0x0008 set) -> put_data(0) -> put_command(0x1600)+doorbell(base+6)
//   out base+4, 6 ; out base+4, 0          ; DMA_sync, 0
//   wait_flush                             ; wait for 0x0400 cleared
// Finally we acknowledge to JAWS with ^A (0x01), exactly as the DECtalk Express
// does when it receives ^C over the serial link.
void dtpc_state::build_flush_actions() {
	m_acts.clear(); m_aidx = 0; m_aoff = 0; m_settle = 0;
	m_acts.push_back({A_WAIT_DSTAT, {},          nullptr});
	m_acts.push_back({A_WAIT_CSTAT, {},          nullptr});
	m_acts.push_back({A_PUTDATA,    {0x00,0x00}, nullptr});  // put_data(0)
	m_acts.push_back({A_PUTCMD,     {0x00,0x16}, nullptr});  // put_command(0x1600) + doorbell
	m_acts.push_back({A_SENDRAW,    {0x06,0x00}, nullptr});  // DMA_sync(6), 0 -> base+4
	m_acts.push_back({A_ACK,        {},          nullptr});  // ^A -> JAWS (acknowledgement, prompt)
	m_acts.push_back({A_WAIT_FLUSH, {},          nullptr});  // let the card finish the flush
	m_flush_count++;
	m_log.line("FLUSH #%ld (^C from JAWS): aborting speech via command 0x1600 + doorbell (INT1), DMA-sync, acknowledging ^A.",
		m_flush_count);
}
bool dtpc_state::run_actions() {
	if (m_aidx >= m_acts.size()) return true;
	action &a = m_acts[m_aidx];
	auto advance = [&]() { m_aidx++; m_aoff = 0; m_settle = 0; };
	switch (a.type) {
	case A_NOTE:
		if (a.note) m_log.line("%s", a.note);
		advance();
		break;
	case A_WAIT: {
		uint16_t flop = hp_status() & 0x0100;
		if (!g_quiet && m_settle > 2000) m_pch[(uint32_t)m_cpu->state_int(STATE_GENPC)]++;
		if (flop != m_flop) { m_flop = flop; m_flop_skips = 0; m_pch.clear(); advance(); }
		else if (++m_settle >= 12000) {
			m_settle = 0;
			if (m_phase == 2) {
				// ---- RUNTIME: bounded wait, directed by DMA1's actual state ----
				// The card's host I/O (KERNEL single_out, loc_119D8) arms DMA1 in two directions:
				//   RECEIVE opcode (host->card):  ctrl 0xA346  (DEST_MIO=0x8000 set)
				//   SEND    byte   (card->host):  ctrl 0x1786  (SRC_MIO=0x1000 set, DEST_MIO clear)
				// (Bit names verified against MAME i186.cpp: SRC_MIO=0x1000, DEST_MIO=0x8000.)
				// Three cases on timeout:
				//
				// (A) GET, opcode written (m_aidx>=2), and DMA1 is in SEND: the card HAS
				//     processed the opcode, put the byte in the latch and XORed the flop (sub_13E5E)
				//     - we just hit a flop value equal to m_flop. It is now waiting on the host
				//     READ, which frees its output DMA. We complete the read.
				//
				// (B) GET, opcode written, but DMA1 is still in RECEIVE: the card has NOT
				//     yet processed the opcode (busy with DSP synthesis; the DMA1-done
				//     ISR has not gotten CPU time). READING here would trigger a dma_sync_req
				//     against a receive DMA and corrupt the card's buffer (= the bug that gave
				//     the 0x02 flood). DROPPING would leave the card mid-handshake. We
				//     therefore WAIT patiently; the card toggles the flop when the ISR runs
				//     (seen by the normal A_WAIT branch). Hard cap only as the very last resort.
				//
				// (C) SEND, or GET before the opcode is written (first A_WAIT): nothing sent
				//     to base+4 -> clean abort. We NEVER write an opcode without a toggle.
				if (++m_flop_skips >= m_rt_wait_limit) {
					const int DXCON1 = I8086_HALT + 13 + 1;          // D1CON
					uint16_t con1 = (uint16_t)m_cpu->state_int(DXCON1);
					bool dma1_send = (con1 & 0x1000) && !(con1 & 0x8000); // SRC_MIO sat, DEST_MIO clear
					if (m_rt_mode == 2 && m_aidx >= 2 && dma1_send) {
						// (A)
						m_flop = flop; m_flop_skips = 0;
						advance();                                   // -> A_READ1
						if (!g_quiet)
							m_log.line("Runtime: GET byte ready in the latch, flop coincided (DMA1-CON=0x%04X) - completing the read.", con1);
					} else if (m_rt_mode == 2 && m_aidx >= 2) {
						// (B) DMA1 still receiving/processing -> wait patiently
						if (m_flop_skips >= m_rt_get_wait_max) {
							m_flop_skips = 0; m_rt_abort = true; m_aidx = m_acts.size();
							m_log.line("WARNING: GET stuck - the card never processed the opcode (DMA1-CON=0x%04X, status 0x%04X). Aborting as a last resort.",
								con1, hp_status());
						} else if (!g_quiet && (m_flop_skips % 20) == 0) {
							m_log.line("Runtime: awaiting the card's processing of GET (DMA1-CON=0x%04X, status 0x%04X, wait=%d).",
								con1, hp_status(), m_flop_skips);
						}
						// otherwise: keep waiting (m_flop_skips keeps growing)
					} else {
						// (C) clean abort, nothing written
						m_flop_skips = 0; m_rt_abort = true; m_aidx = m_acts.size();
						if (!g_quiet)
							m_log.line("Runtime: no flop toggle (status 0x%04X, mode=%d, DMA1-CON=0x%04X) - aborting cleanly.",
								hp_status(), m_rt_mode, con1);
					}
				}
			} else {
				// ---- DOWNLOAD (phase 0/1): a silent card is a REAL error ----
				// NEVER send bytes into a card that has not invited them -
				// it only corrupts the kernel's state. Profile and abort cleanly.
				m_flop_skips++;
				if (m_flop_skips <= 8) {
				// PC profile: which addresses is the CPU spending time on?
				std::vector<std::pair<int,uint32_t>> top;
				for (auto &kv : m_pch) top.push_back(std::make_pair(kv.second, kv.first));
				std::sort(top.rbegin(), top.rend());
				std::string prof;
				for (size_t i = 0; i < top.size() && i < 5; i++) {
					char one[40];
					snprintf(one, sizeof(one), " 0x%05X(x%d)", top[i].second, top[i].first);
					prof += one;
				}
				m_log.line("Waiting for flop toggle (status 0x%04X). 186 PC profile:%s",
					hp_status(), prof.c_str());
				m_log.line("DSP chain: bio-polls=%ld, fed=%ld, consumed=%ld, DAC=%ld, DSP-PC=0x%04X, ctl-writes=%ld.",
					m_bio_polls, m_dsp_feed, m_dsp_consume, m_dac_writes,
					(uint32_t)m_dsp->state_int(STATE_GENPC), m_ctl_writes);
				// i186.h's state enum is protected; the indices are computed from the
				// public I8086_HALT (layout: RELREG=+1, UMCS..MPCS=+2..+6,
				// DxSRC=+7, DxDST=+9, DxTC=+11, DxCON=+13; +1 for channel 1).
				const int DXSRC = I8086_HALT + 7, DXDST = I8086_HALT + 9;
				const int DXTC  = I8086_HALT + 11, DXCON = I8086_HALT + 13;
				m_log.line("186-DMA0: SRC=0x%05X DST=0x%05X TC=0x%04X CON=0x%04X | DMA1: TC=0x%04X CON=0x%04X",
					(uint32_t)m_cpu->state_int(DXSRC+0), (uint32_t)m_cpu->state_int(DXDST+0),
					(uint32_t)m_cpu->state_int(DXTC+0),  (uint32_t)m_cpu->state_int(DXCON+0),
					(uint32_t)m_cpu->state_int(DXTC+1),  (uint32_t)m_cpu->state_int(DXCON+1));
				m_pch.clear();
			}
			if (m_flop_skips == 8) {
				uint32_t pc = (uint32_t)m_cpu->state_int(STATE_GENPC);
				m_log.line("The card no longer responds. Aborting module download CLEANLY");
				m_log.line("(no bytes sent out of turn). Last status 0x%04X, PC 0x%05X.",
					hp_status(), pc);
				m_log.line("The PC value shows where the card's CPU is stuck.");
				{
					auto &spc = m_cpu->space(AS_PROGRAM);
					m_log.line("INT31 vector in IVT: %04X:%04X. Bytes at last ALLOC 0x%05X: %02X %02X %02X %02X.",
						spc.read_word(0x31*4+2), spc.read_word(0x31*4), m_alloc_lin,
						spc.read_byte(m_alloc_lin), spc.read_byte(m_alloc_lin+1),
						spc.read_byte(m_alloc_lin+2), spc.read_byte(m_alloc_lin+3));
				}
				m_acts.clear(); m_aidx = 0;
				m_phase = 2;
				}
			}
		}
		break; }
	case A_SEND: {
		if (m_aoff < a.bytes.size()) {
			// Self-pacing: send a byte ONLY when the card's DMA1 channel is actually
			// armed (ST_STOP=0x0002 in the control word). If we write to a stopped
			// channel, the kernel's drq_callback drops the byte ("channel stopped") and
			// we get cumulative load corruption. The control word is read as
			// DMA1-CON = I8086_HALT+13 +1 (channel 1).
			const int DXCON1 = I8086_HALT + 13 + 1;
			uint16_t con1 = (uint16_t)m_cpu->state_int(DXCON1);
			if (!(con1 & 0x0002)) {
				// channel not ready yet - wait (counts as a stall, not progress)
				if (++m_settle > 12000) {
					m_stall_ticks++;
					m_settle = 0;
					if (m_stall_ticks <= 6)
						m_log.line("  (waiting for DMA1 to be armed before byte %zu/%zu; CON=0x%04X)",
							m_aoff, a.bytes.size(), con1);
				}
				break;
			}
			m_settle = 0;
			// Diagnostics: log DMA1's actual state for the FIRST 8 data bytes
			// in the first big chunk, so we see whether the kernel arms
			// the channel with a sensible destination/count while we stream.
			if (!g_quiet && a.bytes.size() > 256 && m_aoff < 8) {
				const int DSRC1=I8086_HALT+7+1, DDST1=I8086_HALT+9+1, DTC1=I8086_HALT+11+1;
				m_log.line("  [A_SEND byte %zu] DMA1 SRC=0x%05X DST=0x%05X TC=0x%04X CON=0x%04X",
					m_aoff,
					(uint32_t)m_cpu->state_int(DSRC1), (uint32_t)m_cpu->state_int(DDST1),
					(uint32_t)m_cpu->state_int(DTC1),  (uint32_t)con1);
			}
			dput(a.bytes[m_aoff++]);
			// FAST-LOAD: write the rest of the block in THIS tick. Each host_w(4)
			// triggers the i186's drq_callback SYNCHRONOUSLY (the byte is read and written
			// to card RAM immediately, cf. i186.cpp). There is therefore no reason
			// to wait for the card between bytes - it is exactly the tight
			// "while(i--) outp(DT_DMA,*ppb++)" loop the original DT_LOAD uses.
			// We stop only if DMA1 stops (ST_STOP clears) before the block is
			// finished (the channel re-arms on the next tick). Counting count==0 happens
			// exactly when m_aoff reaches the block size, so we never overrun.
			while (m_aoff < a.bytes.size()) {
				con1 = (uint16_t)m_cpu->state_int(DXCON1);
				if (!(con1 & 0x0002)) break;     // DMA1 stopped -> wait for re-arm next tick
				dput(a.bytes[m_aoff++]);
			}
			if (!g_quiet && (m_aoff & 0xfff) == 0)
				m_log.line("  ... %zu/%zu bytes", m_aoff, a.bytes.size());
		} else {
			if (a.note) m_log.line("%s (%zu bytes)", a.note, a.bytes.size());
			advance();
		}
		break; }
	case A_READ1:
		m_rd4[0] = host_r(4);
		advance();
		break;
	case A_SENDRAW:
		// Runtime character send (SEND_CHAR/GET_CHAR): write bytes DIRECTLY to
		// base+4 in a row, WITHOUT DMA1 gating. DMA is used only for LOAD_MEM during load;
		// runtime characters are written as ordinary port writes, exactly as
		// DT_DRIV's SEND_CHAR handler does (opcode + character to base+4 in a row).
		for (uint8_t b : a.bytes) dput(b);
		advance();
		break;
	case A_VERIFY: {
		// Check against the card's RAM BEFORE start: start/middle/end of the sent image.
		auto &spc = m_cpu->space(AS_PROGRAM);
		const std::vector<uint8_t> &ref = m_cur;
		bool ok = true; uint32_t bad = 0;
		size_t probes[3] = {0, ref.size()/2, ref.size()>16 ? ref.size()-16 : 0};
		for (size_t p : probes) {
			for (size_t i = p; i < p+16 && i < ref.size(); i++) {
				if (spc.read_byte(m_alloc_lin+(uint32_t)i) != ref[i]) { ok=false; bad=(uint32_t)i; break; }
			}
			if (!ok) break;
		}
		if (ok) m_log.line("RAM verification BEFORE start: OK (image intact at 0x%05X).", m_alloc_lin);
		else {
			m_log.line("RAM verification BEFORE start FAILED at offset 0x%05X:", bad);
			m_log.line("  sent: %02X %02X %02X %02X - in RAM: %02X %02X %02X %02X",
				ref[bad], ref[bad+1], ref[bad+2], ref[bad+3],
				spc.read_byte(m_alloc_lin+bad), spc.read_byte(m_alloc_lin+bad+1),
				spc.read_byte(m_alloc_lin+bad+2), spc.read_byte(m_alloc_lin+bad+3));
		}
		m_log.line("  latch: host-writes=%ld, card-reads=%ld, collisions=%ld (host-card diff=%ld).",
			m_latch_writes, m_latch_reads, m_latch_collisions, m_latch_writes - m_latch_reads);
		advance();
		break; }
	case A_READ4:
		if (m_aoff < 4) m_rd4[m_aoff++] = host_r(4);
		else {
			m_alloc_lin = (uint32_t)m_rd4[0] | ((uint32_t)m_rd4[1] << 8)
			            | ((uint32_t)m_rd4[2] << 4) | ((uint32_t)m_rd4[3] << 12);
			m_log.line("ALLOC returned address 0x%05X (bytes %02X %02X %02X %02X).",
				m_alloc_lin, m_rd4[0], m_rd4[1], m_rd4[2], m_rd4[3]);
			advance();
		}
		break;
	// ---- abort/flush command path (matches 4.2 DT_DRIV's FLUSH_TEXT exactly) ----
	// Note: timeouts continue (advance) instead of hanging - during active
	// speech the flop and 0x0008/0x0400 change within a few ticks, so they never fire
	// in practice; they are only a safety valve if the card is silent/idle.
	case A_WAIT_DSTAT: {
		uint16_t flop = hp_status() & STAT_FLOP;          // 0x0100
		if (flop != m_flop) { m_flop = flop; advance(); }
		else if (++m_settle >= 40000) { m_settle = 0; advance(); }
		break; }
	case A_WAIT_CSTAT:
		if (hp_status() & STAT_CMD_ACK) advance();         // 0x0008 set = ready for a command
		else if (++m_settle >= 40000) { m_settle = 0; advance(); }
		break;
	case A_PUTDATA:                                        // put_data(): base+2/+3
		host_w(2, a.bytes[0]); host_w(3, a.bytes[1]);
		advance();
		break;
	case A_PUTCMD:                                         // put_command(): base+0/+1 + doorbell base+6
		if (m_aoff == 0) {
			host_w(0, a.bytes[0]); host_w(1, a.bytes[1]); // command word
			host_w(6, 0);                                 // ring doorbell -> INT1 on the 80186 (asynchronous interrupt)
			m_aoff = 1; m_settle = 0;
		} else if (!(hp_status() & STAT_CMD_ACK)) {        // 0x0008 cleared = kernel has consumed the command
			host_w(0, 0); host_w(1, 0);                   // clear the command register (like DT_DRIV)
			advance();
		} else if (++m_settle >= 40000) {
			host_w(0, 0); host_w(1, 0);
			m_settle = 0; advance();
		}
		break;
	case A_WAIT_FLUSH:
		if (!(hp_status() & 0x0400)) advance();            // 0x0400 cleared = flush finished
		else if (++m_settle >= 40000) { m_settle = 0; advance(); }
		break;
	case A_ACK: {                                          // ^A (SOH, 0x01) -> JAWS' Silence() waits for this
		uint8_t ack = 0x01;
		m_serial.write_bytes(&ack, 1);
		advance();
		break; }
	}
	return m_aidx >= m_acts.size();
}

// ---------- download state machine: init handshake + KERNEL download ----------
void dtpc_state::download_tick() {
	if (m_mods.empty()) { m_log.line("No modules - skipping download."); m_phase=2; return; }
	module_t &K=m_mods[0];

	switch (m_dl) {
	// ---- Init handshake on the command register (recovered from DT_LOAD.EXE) ----
	case 0: // the card is at 0x0DEC (ready). Send CMD_GO, then CMD_A.
		m_log.line("INIT: card ready (0x%04X). Sending 0x%04X.", hp_status(), CMD_GO);
		put_command(CMD_GO);
		m_settle=0; m_dl=1; break;
	case 1: // short pause so the card processes, then send 0x5000
		if (++m_settle>=80) { m_log.line("INIT: sending 0x%04X.",CMD_A); put_command(CMD_A); m_dl=2; }
		break;
	case 2: // wait for ack 0x8000
		if (m_stat==ST_ACK1) { m_log.line("INIT: ack 0x%04X. Sending 0x%04X.",ST_ACK1,CMD_B); put_command(CMD_B); m_dl=3; }
		break;
	case 3: // wait for ack 0xF000
		if (m_stat==ST_ACK2) { m_log.line("INIT: ack 0x%04X. Sending 0x%04X.",ST_ACK2,CMD_A); put_command(CMD_A); m_dl=4; }
		break;
	case 4: // wait for ack 0x8000 -> sync done; send CMD_LOAD (0x4000)
		if (m_stat==ST_ACK1) {
			m_log.line("INIT HANDSHAKE DONE (ack 0x%04X). Sending 0x%04X.",ST_ACK1,CMD_LOAD);
			put_command(CMD_LOAD);
			m_dl=5;
		}
		break;
	case 5: // wait for dma_ready (0x10) -> send LOAD_MEM header (5,6)
		if (hp_status()&STAT_DMA_READY) {
			m_log.line("dma_ready (0x10). Sending LOAD_MEM header (5,6).");
			relocate(K,0x40,m_cur);
			m_tx.clear(); m_tx.push_back(CTRL); m_tx.push_back(OP_LOAD_MEM);
			m_txoff=0; m_dl=6;
		}
		break;
	case 6: // send 5,6 (one per tick) -> wait
		if (m_txoff<m_tx.size()) dput(m_tx[m_txoff++]); else m_dl=7;
		break;
	case 7: // wait 0x10 -> send address 0x400 + count
		if (hp_status()&STAT_DMA_READY) {
			uint16_t cnt=(uint16_t)m_cur.size();
			m_tx.clear();
			m_tx.push_back(0x00); m_tx.push_back(0x04); m_tx.push_back(0x00); m_tx.push_back(0x00);
			m_tx.push_back(cnt&0xff); m_tx.push_back(cnt>>8);
			m_txoff=0; m_dl=8;
			m_log.line("Sending address 0x400 + count=%u.",cnt);
		}
		break;
	case 8: // send address+count -> wait
		if (m_txoff<m_tx.size()) dput(m_tx[m_txoff++]); else m_dl=9;
		break;
	case 9: // wait 0x10 -> stream KERNEL image as data
		if (hp_status()&STAT_DMA_READY) {
			m_tx=m_cur; m_txoff=0; m_dl=10;
			m_log.line("Sending KERNEL image (%zu bytes)...",m_tx.size());
		}
		break;
	case 10: // stream KERNEL image - FAST: the whole block in one tick (drq_callback is synchronous)
		if (m_txoff<m_tx.size()) {
			const int DXCON1 = I8086_HALT + 13 + 1;
			uint16_t con1 = (uint16_t)m_cpu->state_int(DXCON1);
			if (con1 & 0x0002) {
				// DMA1 armed -> blast all remaining bytes now (each dput is DMA'd synchronously)
				while (m_txoff<m_tx.size()) {
					con1 = (uint16_t)m_cpu->state_int(DXCON1);
					if (!(con1 & 0x0002)) break;   // DMA1 stopped -> wait for re-arm next tick
					dput(m_tx[m_txoff++]);
				}
			} else {
				// unexpected: DMA1 not armed -> fall back to 1 byte/tick (old, safe behavior)
				dput(m_tx[m_txoff++]);
			}
			if (!g_quiet && (m_txoff & 0x7ff)==0) m_log.line("  ... %zu/%zu bytes",m_txoff,m_tx.size());
		} else {
			m_log.line("KERNEL image sent. Status now 0x%04X.",hp_status());
			m_dl=11;
		}
		break;
	// ---- START_TASK (exactly like DT_LOAD): re-sync, arm, 5,5, wait, ip/cs ----
	case 11: // re-sync: PutCommand(0x5000), wait for 0x8000
		m_log.line("Re-sync: sending 0x%04X (waiting 0x%04X).",CMD_A,ST_ACK1);
		put_command(CMD_A);
		m_dl=12;
		break;
	case 12:
		if (m_stat==ST_ACK1) {
			m_log.line("Re-sync ack. Arming START: sending 0x%04X (waiting 0x10).",CMD_START);
			put_command(CMD_START);
			m_dl=13;
		}
		break;
	case 13: // wait 0x10 -> send 5,5
		if (hp_status()&STAT_DMA_READY) {
			m_tx.clear(); m_tx.push_back(CTRL); m_tx.push_back(OP_START); m_txoff=0; m_dl=14;
			m_log.line("Sending START_TASK (5,5).");
		}
		break;
	case 14: // send 5,5 -> wait
		if (m_txoff<m_tx.size()) dput(m_tx[m_txoff++]); else m_dl=15;
		break;
	case 15: // wait 0x10 -> send ip,cs (cs += 0x40)
		if (hp_status()&STAT_DMA_READY) {
			uint16_t cs=K.cs+0x40, ip=K.ip;
			m_tx.clear();
			m_tx.push_back(ip&0xff); m_tx.push_back(ip>>8); m_tx.push_back(cs&0xff); m_tx.push_back(cs>>8);
			m_txoff=0; m_dl=16;
			m_log.line("KERNEL started at %04X:%04X.",cs,ip);
		}
		break;
	case 16: // send ip,cs -> observe
		if (m_txoff<m_tx.size()) dput(m_tx[m_txoff++]); else { m_settle=0; m_dl=17; }
		break;
	case 17: // observe the card's reaction to kernel start
		if (++m_settle==1000) {
			m_log.line("Status after kernel start: 0x%04X (status written %ld times in total).",hp_status(),m_stat_writes);
			if (hp_status()==0x1000)
				m_log.line("The kernel reported running (0x1000) - THE BOOTSTRAP PROTOCOL WORKS.");
			// DT_LOAD's last step: the 0x6000 command puts the kernel in
			// command mode; it acknowledges with status bit 0x0008.
			m_log.line("Sending GO command 0x6000 (waiting for status bit 0x0008).");
			put_command(CMD_B);
			m_settle=0;
			m_dl=30;
		}
		break;
	case 30: // wait for bit 0x0008 (kernel accepted 0x6000)
		if (hp_status() & STAT_CMD_ACK) {
			m_log.line("The kernel acknowledged (status 0x%04X). Command mode active.",hp_status());
			m_flop = hp_status() & STAT_FLOP;
			m_flop_skips = 0;
			m_ordidx = 0; m_xstage = 0;
			m_dl = 18;
		} else if (++m_settle==12000) {
			m_log.line("No acknowledgement of 0x6000 yet (status 0x%04X) - ringing the doorbell (base+6).",hp_status());
			host_w(6,1); // INT1 to the card's 80186
		} else if (m_settle==24000) {
			m_log.line("Still no acknowledgement (status 0x%04X). Continuing with the download anyway.",hp_status());
			m_flop = hp_status() & STAT_FLOP;
			m_flop_skips = 0;
			m_ordidx = 0; m_xstage = 0;
			m_dl = 18;
		}
		break;
	// ---- module download in kernel mode ----
	case 18: { // choose the next module in the order
		static const char *order[] = {"dtpcdic.dic","lts.exe","ph.exe","cmd.exe","dtpc_850.exe","usa.exe"};
		module_t *M = nullptr;
		while (m_ordidx < 6 && !(M = find_mod(order[m_ordidx]))) {
			m_log.line("(skipping %s - not loaded)", order[m_ordidx]);
			m_ordidx++;
		}
		if (!M) {
			m_log.line("ALL MODULES AND THE DICTIONARY HAVE BEEN SENT.");
			// All modules are loaded; go to runtime mode (case 40) and queue
			// the startup message.
			m_dl = 40;
			m_settle = 0;
			break;
		}
		m_log.line("=== Module %zu/6: %s (%zu bytes) ===", m_ordidx+1, M->name.c_str(),
			M->is_mz ? M->image.size() : M->file.size());
		build_alloc_actions(*M);
		m_xstage = 0;
		m_dl = 19;
		break; }
	case 19: { // run the action list (ALLOC, then XFER)
		if (!run_actions()) break;
		static const char *order[] = {"dtpcdic.dic","lts.exe","ph.exe","cmd.exe","dtpc_850.exe","usa.exe"};
		module_t *M = find_mod(order[m_ordidx]);
		if (!M) { m_dl=18; break; } // should not happen
		if (m_xstage == 0) {
			if ((m_alloc_lin & 0xf) != 0)
				m_log.line("WARNING: ALLOC address 0x%05X is not paragraph-aligned.", m_alloc_lin);
			bool is_dict = (M->name == "dtpcdic.dic");
			build_xfer_actions(*M, is_dict);
			m_xstage = 1;
		} else {
			auto &spc = m_cpu->space(AS_PROGRAM);
			m_log.line("%s done (status 0x%04X, PC 0x%05X, INT31 vector %04X:%04X).",
				M->name.c_str(), hp_status(), (uint32_t)m_cpu->state_int(STATE_GENPC),
				spc.read_word(0x31*4+2), spc.read_word(0x31*4));
			m_ordidx++;
			m_dl = 18;
		}
		break; }
	case 40: { // Go to runtime mode and queue the startup message.
		m_phase = 2;
		// The runtime pulse period = how often we POLL the card's flop bit and feed
		// characters. The real DT_DRIV (SEND_CHAR, non-DTEX) has NO fixed delay:
		// it tries in a TIGHT CPU loop and sends a character each time the card's
		// flop bit toggles (the card/firmware sets the pace itself via the status
		// register). The faithful reproduction is therefore as short a pulse as useful -
		// we want to catch every flop toggle without delay. 5 us (200 kHz) polls
		// far faster than the card can toggle the flop, so both character feeding AND
		// restart after a flush are as quick as the emulation allows.
		// (The synthesis delay itself - most of the ~500 ms - lies in the DSP
		// pipeline, which runs below real time because the DSP clock must stay at
		// 80 MHz; it is not affected by this pulse.) Adjust via DTPC_PUMPUS if needed.
		unsigned pump_us = 5;
		if (const char *e = getenv("DTPC_PUMPUS")) { unsigned v = atoi(e); if (v >= 2 && v <= 500) pump_us = v; }
		m_pump->adjust(attotime::from_usec(pump_us),0,attotime::from_usec(pump_us));
		m_log.line("Switched to runtime pulse of %u us (matches DT_DRIV's tight send loop; adjust via DTPC_PUMPUS).", pump_us);
		// The DECtalk PC starts at VERY low volume; you must turn it up yourself.
		// FreeDOS' dtstart sends "[:volume set 100]". We do the same and
		// add an audible startup confirmation, so the user knows the card is ready.
		// Inline commands in [] brackets are interpreted by the card (TTS command language).
		{
			static const char msg[] = "[:volume set 100]DECtalk PC is running.";
			for (const char *p = msg; *p; ++p) m_rtq.push_back((uint8_t)*p);
			m_rtq.push_back('\r'); // end the utterance so the card speaks it
			m_log.line("Queued startup message: volume 100 + \"DECtalk PC is running.\"");
		}
		m_log.line("Status now 0x%04X. Open your speech program on the other COM port and send text.",hp_status());
		break; }
	}
}

TIMER_CALLBACK_MEMBER(dtpc_state::pump_tick) {
	// Measurement: compare DAC samples per WALL-CLOCK second with emulated time.
	// Determine whether the card produces audio in real time (~10kHz) or too slowly.
	if (!g_quiet && m_phase==2 && m_rt_tx_count>0) {
		auto now = std::chrono::steady_clock::now();
		if (!m_wall_init) {
			m_wall0 = now; m_dac_at_mark = m_dac_writes;
			m_emu_at_mark = machine().time().as_double(); m_wall_init = true;
		} else {
			double wall = std::chrono::duration<double>(now - m_wall0).count();
			if (wall >= 1.0) {
				long dac = m_dac_writes - m_dac_at_mark;
				double emu = machine().time().as_double() - m_emu_at_mark;
				m_log.line("MEASUREMENT: %ld DAC samples in %.2f wall-clock sec (%.0f/sec), %.3f emulated sec elapsed (emu/wall=%.2f).",
					dac, wall, dac/wall, emu, emu/wall);
				m_log.flush_now();
				m_wall0 = now; m_dac_at_mark = m_dac_writes;
				m_emu_at_mark = machine().time().as_double();
			}
		}
	}

	if (m_phase==0) { // wait for POST + READY signal (0x0DEC)
		m_wd_ticks++;
		if (m_stat_writes==0 && m_wd_ticks==3000) {
			m_log.line("ERROR: the card has NOT written status in ~0.3 s.");
			m_log.line("The CPU is not running the boot ROM. Check the three ROM files in roms/dtpc/");
			m_log.line("and that the build succeeded without errors.");
			m_phase=3; return;
		}
		if (m_stat==ST_READY) { // 0x0DEC - POST done, card ready
			m_log.line("The card reached READY state 0x%04X after POST (status written %ld times).",ST_READY,m_stat_writes);
			m_log.line("80186 + TMS320C15 + glue work. Starting init handshake + download.");
			m_phase=1; m_dl=0; m_wd_ticks=0; m_wd_sig=-1;
			return;
		}
		if (m_wd_ticks==160000) { // ~16 s and never 0x0DEC
			m_log.line("The card NEVER reached the ready state 0x%04X. Last status: 0x%04X.",ST_READY,m_stat);
			m_log.line("POST may not have completed.");
			m_phase=3;
		}
		return;
	}
	if (m_phase==1) {
		// Watchdog: detects if a step does not move (locked up).
		unsigned usig = (unsigned)m_dl*100u + (unsigned)m_ordidx;
		usig = usig*1009u + (unsigned)(m_aidx % 1009);
		usig = usig*131071u + (unsigned)((m_txoff + m_aoff) % 131071);
		usig += (unsigned)m_flop_skips * 7919u; // PC sampling counts as progress
		usig += (unsigned)(m_stall_ticks & 0xffff) * 6037u; // DMA1 wait counts as progress
		int sig = (int)usig;
		if (sig != m_wd_sig) { m_wd_sig=sig; m_wd_ticks=0; }
		else if (++m_wd_ticks==30000) { // ~3 s with no progress at all (not even a stall)
			m_log.line("LOCKED UP at step %d (module %zu, action %zu, offset %zu). Card status now 0x%04X.",
				m_dl,m_ordidx,m_aidx,m_txoff+m_aoff,hp_status());
			m_log.line("DMA1 was never armed (total stall waits: %ld).", m_stall_ticks);
			m_phase=2; // give up, keep the program responsive
		}
		download_tick();
		return;
	}
	if (m_phase==3) return; // dead

	// runtime: text pipe (exactly like DT_DRIV's SEND_CHAR/GET_CHAR handlers).
	// SEND_CHAR (0x0f8a): [wait flop toggle] write opcode 1 + character to base+4 in a ROW.
	// GET_CHAR  (0x1020): [wait flop toggle] write opcode 2 + 0, [wait flop toggle] read character.
	// Status: bit 0x04 = card can receive; bit 0x02 = card has a character ready.
	if (m_aidx < m_acts.size()) {
		if (run_actions()) {
			if (m_rt_abort) {
				m_rt_abort = false;
				// Aborted runtime handshake: we have NOT written any opcode without a
				// confirmed toggle. Re-queue a SEND character at the FRONT so it is not lost;
				// drop a GET (JAWS polls the index again). Re-evaluate from fresh status.
				if (m_rt_mode == 1 && m_rt_pending_valid)
					m_rtq.insert(m_rtq.begin(), m_rt_pending_char);
				m_rt_pending_valid = false;
				m_acts.clear(); m_aidx = 0; m_aoff = 0;
				m_rt_mode = 0;
				return;
			}
			if (m_rt_mode == 2) {
				uint8_t b = m_rd4[0];
				m_rt_rx_count++;
				// Flood guard: count identical bytes in a row. A card that after a
				// flush spits out the same byte forever (seen: 0x02) is
				// NOT a real index reply. We still drain the card (so its
				// out ring does not fill up), but stop forwarding to
				// JAWS once the flood is detected, so neither JAWS nor the log
				// drowns. A SEND resets the guard (see the send branch).
				if (m_rt_rx_count > 1 && b == m_get_last) m_get_run++;
				else m_get_run = 0;
				m_get_last = b;
				if (m_get_run >= 16) {
					if (!m_get_flood) {
						m_log.line("WARNING: the card repeats byte 0x%02X (>=16 times) after flush - not forwarding the flood to JAWS (draining only).", b);
						m_get_flood = true;
					}
					// do NOT forward - just drain
				} else {
					m_serial.write_bytes(&b, 1);
					if (!m_rt_first_rx) { m_log.line("First character received from the card: 0x%02X.", b); m_rt_first_rx=true; }
					// log the first 32 fetched bytes raw, so we see the card's reply stream
					if (m_rt_rx_count <= 32)
						m_log.line("  <- fetched from card #%ld: 0x%02X ('%c')", m_rt_rx_count, b,
							(b>=32 && b<127) ? b : '.');
				}
			} else if (m_rt_mode == 1) {
				m_rt_pending_valid = false;   // the character is now safely sent (no re-queue)
				if (!m_rt_first_tx) { m_log.line("First character sent to the card."); m_rt_first_tx=true; }
				m_rt_tx_count++;
			} else if (m_rt_mode == 3) {
				m_drain_after_flush_blocked = true;   // empty ring after flush: drain again only on the next SEND
				m_log.line("FLUSH #%ld done (card status 0x%04X). Ready for a new utterance.",
					m_flush_count, hp_status());
			}
			m_rt_mode = 0;
			// periodic combined status: traffic both ways + DSP chain
			if (((m_rt_tx_count + m_rt_rx_count) % 16) == 0) {
				m_log.line("Runtime traffic: sent=%ld, fetched=%ld, queue-remaining=%zu.",
					m_rt_tx_count, m_rt_rx_count, m_rtq.size());
				m_log.line("  DSP chain: bio-polls=%ld, fed=%ld, consumed=%ld, DAC=%ld, DSP-PC=0x%04X.",
					m_bio_polls, m_dsp_feed, m_dsp_consume, m_dac_writes,
					(uint32_t)m_dsp->state_int(STATE_GENPC));
			}
		}
		return;
	}
	// engine idle: fill the queue from the serial port.
	// IMPORTANT: ^C (0x03) is NOT text - it is JAWS' abort/flush signal from
	// the DECtalk Express serial protocol (confirmed in dte.jls: Silence() sends
	// PurgeComm + ^C and waits for ^A). It must NEVER be sent as a character to the card;
	// instead it triggers a card flush via the command register + doorbell (INT1),
	// exactly like the original DT_DRIV's FLUSH_TEXT.
	uint8_t buf[256];
	int n = m_serial.read_available(buf, sizeof(buf));
	if (n > 0) {
		for (int i = 0; i < n; i++) {
			uint8_t b = buf[i];
			if (b == 0x03) {                 // ^C = abort/flush
				m_rtq.clear();               // discard unsent old text (equivalent to JAWS' PurgeComm)
				m_flush_pending = true;
			} else {
				m_rtq.push_back(b);
			}
		}
		if (!m_rt_first_in) { m_log.line("Received %d characters from the serial port (queue length now %zu).", n, m_rtq.size()); m_rt_first_in=true; }
	}
	// Flush has the highest priority: abort ongoing speech BEFORE we send/fetch characters.
	if (m_flush_pending) {
		m_flush_pending = false;
		build_flush_actions();
		m_rt_mode = 3;
		return;
	}
	uint16_t st = hp_status();
	// PRIORITIZE SEND: empty the text queue FIRST, so new text (e.g. the utterance JAWS
	// sends right after a flush/^C) actually gets spoken.
	if (!m_rtq.empty() && (st & STAT_RR_CHAR) && dstat_ready()) {
		uint8_t ch = m_rtq.front();
		m_rtq.erase(m_rtq.begin());
		m_rt_pending_char = ch; m_rt_pending_valid = true; // for re-queue if the handshake is aborted
		m_acts.clear(); m_aidx=0; m_aoff=0;
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_SENDRAW, {DMA_SINGLE_IN, ch}, nullptr}); // opcode 1 + character, in a row
		m_rt_mode = 1;
		m_get_run = 0; m_get_flood = false;   // new text sent -> reset the flood guard
		m_drain_after_flush_blocked = false;  // new text -> the card may now have real output to drain
		if (m_rtq.empty())
			m_log.line("Entire queue sent to the card (total sent now %ld). Awaiting DSP/synthesis.", m_rt_tx_count+1);
		return;
	}
	// Drain the card's output (index replies etc.) to JAWS - only when there is no text to
	// send. We still fetch (so the card's out ring does not fill up), but a
	// flood guard ensures that a card spitting out the same byte forever
	// does NOT flood JAWS or the log file (that is never a real index reply).
	// Two guards against the self-feeding 0x02 flood on an empty ring: (1) right after a
	// flush nothing is drained until the next SEND; (2) if a flood is already detected,
	// we stop draining (not just forwarding) until the next SEND resets it.
	if ((st & STAT_TR_CHAR) && dstat_ready() && !m_drain_after_flush_blocked && !m_get_flood) {
		m_acts.clear(); m_aidx=0; m_aoff=0;
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_SENDRAW, {DMA_SINGLE_OUT, 0x00}, nullptr}); // opcode 2 + 0
		m_acts.push_back({A_WAIT, {}, nullptr});
		m_acts.push_back({A_READ1, {}, nullptr});
		m_rt_mode = 2;
		return;
	}
	// Idle: keep an eye on whether the DSP chain works after the text is delivered.
	if (!g_quiet && m_rtq.empty() && m_rt_tx_count > 0) {
		if (++m_rt_idle == 20000) {
			m_log.line("Idle: DSP chain bio-polls=%ld, fed=%ld, consumed=%ld, DAC=%ld, DSP-PC=0x%04X, status=0x%04X.",
				m_bio_polls, m_dsp_feed, m_dsp_consume, m_dac_writes,
				(uint32_t)m_dsp->state_int(STATE_GENPC), hp_status());
			m_rt_idle = 0;
		}
	}
}

void dtpc_state::machine_start() {
	m_log.open("dtpc.log");
	m_log.line("DECtalk PC emulator starting.");
	m_pump=timer_alloc(FUNC(dtpc_state::pump_tick),this);
	const char *port=getenv("DTPC_PORT");
	m_log.line("Serial port: %s",(port&&*port)?port:"(PTY)");
	if (!m_serial.open(port?port:"",9600)) m_log.line("WARNING: could not open the serial port.");
	else if (!m_serial.pty_name().empty()) m_log.line("PTY created - your program opens: %s",m_serial.pty_name().c_str());
	else m_log.line("Serial port opened OK.");
	load_module_files();
	m_log.line("DSP clock: %u MHz%s.", m_dsp_mhz, m_dsp_mhz==80 ? " (MAME default)" : " (adjusted via DTPC_DSPMHZ)");
	m_log.line("Output gain (DTPC_GAIN): %.3f x  (1.0 = unchanged; 2.0 ~ +6.02 dB, compensates for half scale).", m_out_gain);
	save_item(NAME(m_cmd)); save_item(NAME(m_stat)); save_item(NAME(m_data));
	save_item(NAME(m_dsp_dma)); save_item(NAME(m_ctl)); save_item(NAME(m_dma));
	save_item(NAME(m_vol)); save_item(NAME(m_bio));
}
void dtpc_state::machine_reset() {
	m_ctl=0; m_vol=63; m_bio=ASSERT_LINE; m_last_stat=0xffff;
	m_phase=0; m_dl=0; m_stat_writes=0; m_wd_ticks=0; m_wd_sig=-1;
	m_flush_pending=false;
	m_get_last=0; m_get_run=0; m_get_flood=false;
	m_rt_abort=false; m_rt_pending_valid=false; m_rt_pending_char=0; m_flop_skips=0;
	m_drain_after_flush_blocked=false;
	m_log.line("Reset - boot ROM begins self-test (POST).");
	m_pump->adjust(attotime::from_usec(100),0,attotime::from_usec(100));
}

void dtpc_state::dtpc(machine_config &config) {
	I80186(config,m_cpu,20_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM,&dtpc_state::cpu_map);
	m_cpu->set_addrmap(AS_IO,&dtpc_state::cpu_io);
	m_cpu->tmrout0_handler().set(FUNC(dtpc_state::clock_w));
	// DSP clock: MAME uses 80 MHz (a hack - "to make it work" for the ISA card).
	// It is the heaviest emulation cost. The real crystal is 20 MHz.
	// Lower clock = lighter emulation = less stutter on weak machines, AS LONG AS
	// the DSP still computes every sample in time. Adjust with DTPC_DSPMHZ (MHz).
	unsigned dsp_mhz = 80;
	if (const char *e = getenv("DTPC_DSPMHZ")) { unsigned v = atoi(e); if (v >= 5 && v <= 160) dsp_mhz = v; }
	TMS320C15(config,m_dsp,dsp_mhz * 1'000'000);
	m_dsp_mhz = dsp_mhz;
	m_dsp->set_addrmap(AS_PROGRAM,&dtpc_state::dsp_map);
	m_dsp->set_addrmap(AS_IO,&dtpc_state::dsp_io);
	m_dsp->bio().set(FUNC(dtpc_state::bio_line_r));

	// MAME pumps the audio stream per video frame. Without a screen the machine gets
	// no smooth frame rate, so real-time audio plays in clumps (stutter), while
	// WAV output (not real-time bound) stays clean - exactly what was observed.
	// An invisible high-refresh dummy screen gives a smooth frame anchor, so
	// the audio is pumped regularly. The screen has no visual purpose here.
	screen_device &screen = SCREEN(config, "screen", SCREEN_TYPE_RASTER);
	screen.set_refresh_hz(60);
	screen.set_size(8, 8);
	screen.set_visarea(0, 7, 0, 7);
	screen.set_screen_update(FUNC(dtpc_state::screen_update));

	SPEAKER(config,"speaker").front_center();
	// Extra output gain placed AFTER the card's internal volume (both the software [12C]
	// and the firmware-controlled hardware attenuator m_vol). Placed on the DAC->speaker
	// route, so the card emulation itself is untouched; the route gain is multiplied with
	// output_ctl_w's output gain, so the m_vol control is preserved. 1.0 = unchanged;
	// DTPC_GAIN=2.0 (~ +6.02 dB) compensates for the observed half scale. Clamped 0..8.
	double out_gain = 1.0;
	if (const char *e = getenv("DTPC_GAIN")) { double v = atof(e); if (v >= 0.0 && v <= 8.0) out_gain = v; }
	m_out_gain = out_gain;
	DAC_12BIT_R2R_TWOS_COMPLEMENT(config,m_dac,0).add_route(0,"speaker", out_gain); // AD7541
}

ROM_START( dtpc )
	ROM_REGION( 0x4000, "maincpu", 0 )
	ROM_LOAD16_BYTE("pc_boot_hxl.am27c64.d6.e26", 0x0000, 0x2000, CRC(7492f1e3) SHA1(fe6946a227f01c94f2b99220320a616445c96ee0))
	ROM_LOAD16_BYTE("pc_boot_hxh.am27c64.d8.e27", 0x0001, 0x2000, CRC(1fe7fe40) SHA1(6e89c237f01aa22e0d21ff4d6fdf8137c6ace374))
	ROM_REGION( 0x2000, "dsp", 0 )
	ROM_LOAD("spc_034c__2-1-92.tms320p15nl.d3.bin", 0x0000, 0x2000, CRC(d8b1201e) SHA1(4b873a5e882205fcac79a27562054b5c4d1a117c))
ROM_END

static INPUT_PORTS_START( dtpc )
INPUT_PORTS_END

} // anonymous namespace

//    YEAR  NAME   PARENT  COMPAT  MACHINE  INPUT  CLASS         INIT        COMPANY              FULLNAME      FLAGS
SYST( 1996, dtpc, 0,      0,      dtpc,   dtpc, dtpc_state,  empty_init, "Digital Equipment", "DECtalk PC", MACHINE_NOT_WORKING )
