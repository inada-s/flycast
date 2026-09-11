typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define CALL_ORG_FUNC 0 // works only disk2
#define DEBUG_PRINT 0

#define GDXDATA __attribute__((section("gdx.data")))
#define GDXFUNC __attribute__((section("gdx.func")))
#define GDXMAIN1 __attribute__((section("gdx.main1")))
#define GDXMAIN2 __attribute__((section("gdx.main2")))

// Append each feature after the legacy networking payload (see ld.script).
// Widescreen source order keeps its released entries and table in place.
#define GDXWSDATA __attribute__((section("gdx.data.ws")))
#define GDXWSFUNC __attribute__((section("gdx.func.ws"), no_reorder))
#define GDXSTATSDATA __attribute__((section("gdx.data.stats")))
#define GDXSTATSFUNC __attribute__((section("gdx.func.stats"), noinline))

#if DEBUG_PRINT
#include "mini-printf.h"
#endif

#define read8(a) *((u8*)(a))
#define read16(a) *((u16*)(a))
#define read32(a) *((u32*)(a))
#define write8(a, b) *((u8*)(a)) = (b)
#define write16(a, b) *((u16*)(a)) = (b)
#define write32(a, b) *((u32*)(a)) = (b)

#define BIN_OFFSET 0x80000000

enum {
    GDX_RPC_SOCK_OPEN = 1,
    GDX_RPC_SOCK_CLOSE = 2,
    GDX_RPC_SOCK_READ = 3,
    GDX_RPC_SOCK_WRITE = 4,
    GDX_RPC_SOCK_POLL = 5,
};

struct gdx_rpc_t {
    u32 request;
    u32 response;
    u32 param1;
    u32 param2;
    u32 param3;
    u32 param4;
};

struct hostent {
    u8 *h_name;
    u8 **h_aliases;
    u32 h_addrtype;
    u32 h_length;
    u8 **h_addr_list;
};

struct sockaddr_t {
    u8 sin_len;
    u8 sin_family;
    u16 sin_port;
    u32 sin_addr;
    u8 sin_zero[8];
};

GDXDATA u32 patch_id = 0;
GDXDATA u32 disk = 0;
GDXDATA u32 is_online = 0;
GDXDATA u32 rbk_ex_input = 0;
GDXDATA u32 print_buf_pos = 0;
GDXDATA u8 ppp_status_ok[] = {
        0x01, 0xa7, 0xa8, 0xc0, 0x02, 0xa7, 0xa8, 0xc0,
        0x04, 0x00, 0x00, 0x00, 0x4b, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00};
GDXDATA char print_buf[1024] = {0};
struct hostent host_entry GDXDATA = {0};
u8 *host_addr_list[1] GDXDATA = {0};
u8 host_addr_0[4] GDXDATA = {0};
volatile GDXDATA u8 dummy = 0;
volatile struct gdx_rpc_t gdx_rpc GDXDATA = {0};

void GDXFUNC gdx_printf(const char *format, ...) {
#if DEBUG_PRINT
    va_list arg;
    va_start(arg, format);
    print_buf_pos += mini_vsnprintf(print_buf + print_buf_pos, sizeof(print_buf) - print_buf_pos, format, arg);
    va_end(arg);
#endif
}

#define PRINT_RETURN_ADDR do{ gdx_printf("RETURN ADDR: 0x%08x\n", __builtin_extract_return_addr (__builtin_return_address (0))); } while(0);

u32 GDXFUNC gdx_rpc_call(u32 request, u32 param1, u32 param2, u32 param3, u32 param4) {
    gdx_rpc.request = request;
    gdx_rpc.response = 0;
    gdx_rpc.param1 = param1;
    gdx_rpc.param2 = param2;
    gdx_rpc.param3 = param3;
    gdx_rpc.param4 = param4;
    dummy = read8(0x00400000);
    return gdx_rpc.response;
}

int GDXFUNC gdx_sock_create(int param1, int param2, int param3) {
    gdx_printf("%s %d %d %d\n", __FUNCTION__, param1, param2, param3);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(int, int, int)) 0x0c1a8c08)(param1, param2, param3);
    gdx_printf("sock = %d\n", org_ret);
    return org_ret;
#else
    return 1;
#endif
}

int GDXFUNC gdx_sock_close(int param1) {
    gdx_printf("%s %d\n", __FUNCTION__, param1);

#if CALL_ORG_FUNC
    int org_ret = ((int (*)(int)) 0x0c1a87a0)(param1);
    return org_ret;
#else
    is_online = 0;
    gdx_rpc_call(GDX_RPC_SOCK_CLOSE, param1, 0, 0, 0);
    return 0;
#endif
}

void *GDXFUNC gdx_gethostbyname(const char *param1) {
    gdx_printf("%s %s\n", __FUNCTION__, param1);

#if CALL_ORG_FUNC
    void *org_ret = ((void *(*)(const char *)) 0x0c1a71c0)(param1);
    struct hostent *ent = (struct hostent *) org_ret;
    if (org_ret != 0) {
        gdx_printf("%d\n", ent->h_length);
        if (ent->h_length != 0) {
            gdx_printf("IP = %d.%d.%d.%d\n",
                       (u8) *((ent->h_addr_list[0])),
                       (u8) *((ent->h_addr_list[0]) + 1),
                       (u8) *((ent->h_addr_list[0]) + 2),
                       (u8) *((ent->h_addr_list[0]) + 3));
        }
    }
    return org_ret;
#else
    host_addr_0[0] = 7;
    host_addr_0[1] = 7;
    host_addr_0[2] = 0;
    host_addr_0[3] = 0;
    host_entry.h_name = 0;
    host_entry.h_aliases = 0;
    host_entry.h_addrtype = 2; // AF_INET
    host_entry.h_length = 1;
    host_entry.h_addr_list = host_addr_list;
    host_addr_list[0] = host_addr_0;
    return &host_entry;
#endif
}

int GDXFUNC gdx_connect_sock(int sock, struct sockaddr_t *sock_addr, int len) {
    gdx_printf("%s %d %08x %d\n", __FUNCTION__, sock, sock_addr, len);
    gdx_printf("addr:%08x port:%d\n", sock_addr->sin_addr, sock_addr->sin_port);
#if CALL_ORG_FUNC
    int org_ret = ((void *(*)(int, struct sockaddr_t *, int)) 0x0c1a76b8)(sock, sock_addr, len);
    return org_ret;
#else
    is_online = 1;
    u32 addr = sock_addr->sin_addr;
    u16 port = sock_addr->sin_port;
    port = port >> 8 | (port & 0xff) << 8;
    gdx_rpc_call(GDX_RPC_SOCK_OPEN, addr == 0x0707, addr, port, 0);
    if (addr == 0x0707) {
        return 1;
    } else {
        return 2;
    }
#endif
}

int GDXFUNC gdx_ppp_get_status(int param1, int param2, u8 *param3) {
    gdx_printf("%s %d %d %d\n", __FUNCTION__, param1, param2, param3);

    PRINT_RETURN_ADDR;
#if CALL_ORG_FUNC
    if (param2 != 0) {
        gdx_printf("**param2 (before) = %d\n", read32(read32(param2)));
    }
    int org_ret = ((int (*)(int, int, u8 *)) 0x0c1a8e08)(param1, param2, param3);
    if (param2 != 0) {
        gdx_printf("**param2 (after) = %d\n", read32(read32(param2)));
    }
    if (param1 == 2) {
        if (param3 != 0) {
            gdx_printf("ppp_status(3) 0x%08x:", param3);
            for (int i = 0; i <= 0x12; ++i) {
                gdx_printf(" %02x", read8(param3 + i));
            }
            gdx_printf("\n");
        }
    }
    gdx_printf("org_ret = %d\n", org_ret);
    return org_ret;
#else
    if (param1 == 0 || param1 == 1) {
        gdx_rpc_call(GDX_RPC_SOCK_CLOSE, param1, 1, 0, 0);
    } else if (param1 == 2 || param1 == 3) {
        for (int i = 0; i < sizeof(ppp_status_ok); ++i) {
            param3[i] = ppp_status_ok[i];
        }

        if (disk == 1) {
            if (read8(0x0c2f6639) == 16) {
                write8(0x0c2f6639, 2);
            }
            if (read8(0x0c2f6641)) {
                param3[8] = 0x00;
            }
        }

        if (disk == 2) {
            // back from disconnection
            if (read8(0x0c391d79) == 16) {
                write8(0x0c391d79, 2);
            }
            // logout
            if (read8(0x0c391d81)) {
                param3[8] = 0x00;
            }
        }
        gdx_printf("param3[8] = %d\n", param3[8]);
    } else {
        gdx_printf("return flow %d", param1);
        return 0xffffff9d;
    }
    return 0;
#endif
}

int GDXFUNC gdx_select(u32 param1, void *param2, u32 param3, void *param4, u32 param5) {
    gdx_printf("%s %d %08x %d %d %d\n", __FUNCTION__, param1, param2, param3, param4, param5);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, void *, u32, void *, u32)) 0x0c1a801c)(param1, param2, param3, param4, param5);
    gdx_printf("ret = %d\n", org_ret);
    return org_ret;
#else
    if (is_online) {
        return 0 < gdx_rpc_call(GDX_RPC_SOCK_POLL, 0, 0, 0, 0);
    } else {
        return -1;
    }
#endif
}

int GDXFUNC gdx_lbs_sock_write(u32 sock, u8 *buf, u32 size) {
    gdx_printf("%s %d %08x %d\n", __FUNCTION__, sock, buf, size);
    for (int i = 0; i < size; ++i) {
        gdx_printf("%02x", buf[i]);
    }
    gdx_printf("\n");
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, u8 *, u32)) 0x0c1a8ad4)(sock, buf, size); // call original sock_write function
    return org_ret;
#else
    return gdx_rpc_call(GDX_RPC_SOCK_WRITE, buf, size, 0, 0);
#endif
}


int GDXFUNC gdx_lbs_sock_read(u32 sock, u8 *buf, u32 size) {
    gdx_printf("%s %d %08x %d\n", __FUNCTION__, sock, buf, size);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, u8 *, u32)) 0x0c1a88e4)(sock, buf, size);
    for (int i = 0; i < size; ++i) {
        gdx_printf("%02x", buf[i]);
    }
    gdx_printf("\n");
    return org_ret;
#else
    if (is_online) {
        int n = gdx_rpc_call(GDX_RPC_SOCK_READ, buf, size, 0, 0);
        gdx_printf("write %d", n);
        return n;
    } else {
        return -1;
    }
#endif
}

int GDXFUNC gdx_mcs_sock_read(u32 sock, u8 *buf, u32 size) {
    gdx_printf("%s 0x%x 0x%x %d\n", __FUNCTION__, sock, buf, size);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, u8 *, u32)) 0x0c0455ba)(sock, buf, size); // call original read function
    for (int i = 0; i < org_ret; i++) {
        gdx_printf("%02x", buf[i]);
    }
    gdx_printf("\n");
    return org_ret;
#else
    if (is_online) {
        return gdx_rpc_call(GDX_RPC_SOCK_READ, buf, size, 0, 0);
    } else {
        return -1;
    }
#endif
}

int GDXFUNC gdx_mcs_sock_write(u32 sock, u8 *buf, u32 size, int unk) {
    gdx_printf("%s %d %08x %d %d\n", __FUNCTION__, sock, buf, size, unk);
#if CALL_ORG_FUNC
    int org_ret = ((int (*)(u32, u8 *, u32, int)) 0x0c1a8584)(sock, buf, size, unk);
    for (int i = 0; i < size; ++i) {
        gdx_printf("%02x", buf[i]);
    }
    gdx_printf("\n");
    return org_ret;
#else
    return gdx_rpc_call(GDX_RPC_SOCK_WRITE, buf, size, 0, 0);
#endif
}

int GDXFUNC gdx_softreset_disconnect() {
    gdx_printf("%s\n", __FUNCTION__);

    int ret = 0;
    if (disk == 1) {
        ret = ((int (*)()) 0x0c045c68)();
    }
    if (disk == 2) {
        ret = ((int (*)()) 0x0c03308c)();
    }
    gdx_printf("ret = %d\n", ret);

    if (ret == 1 && is_online) {
        return gdx_rpc_call(GDX_RPC_SOCK_CLOSE, 0, 2, 0, 0);
    }
    return ret;
}

void GDXFUNC gdx_initialize() {
    gdx_printf("gdx_initialize disk = %d\n", disk);
    if (disk == 0) {
        return;
    }

    is_online = 0;

    if (disk == 1) {
        write32(BIN_OFFSET + 0x0c05811c, gdx_sock_create);
        write32(BIN_OFFSET + 0x0c0354d4, gdx_sock_create);
        write32(BIN_OFFSET + 0x0c05843c, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c058128, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c045e08, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c035918, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c035704, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c0355e4, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c0354b8, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c031efc, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c0354e4, gdx_gethostbyname);
        write32(BIN_OFFSET + 0x0c058124, gdx_connect_sock);
        write32(BIN_OFFSET + 0x0c0355e0, gdx_connect_sock);
        write32(BIN_OFFSET + 0x0c058430, gdx_select);
        write32(BIN_OFFSET + 0x0c046a9c, gdx_select);
        write32(BIN_OFFSET + 0x0c14a098, gdx_ppp_get_status);
        write32(BIN_OFFSET + 0x0c046bb4, gdx_lbs_sock_read);
        write32(BIN_OFFSET + 0x0c047040, gdx_lbs_sock_write);
        write32(BIN_OFFSET + 0x0c059290, gdx_mcs_sock_read);
        write32(BIN_OFFSET + 0x0c058438, gdx_mcs_sock_write);
        write32(BIN_OFFSET + 0x0c010a4c, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c010e00, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c01101c, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c064d20, gdx_softreset_disconnect);

        for (u32 p = 0x0c0353f4; p <= 0x0c03540a; p += 2) {
            write16(BIN_OFFSET + p, 0x0009);
        }
    }
    if (disk == 2) {
        write32(BIN_OFFSET + 0x0c0228f8, gdx_sock_create);
        write32(BIN_OFFSET + 0x0c045504, gdx_sock_create);
        write32(BIN_OFFSET + 0x0c01f284, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c0228dc, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c022a08, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c022b28, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c022d3c, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c03322c, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c045510, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c045824, gdx_sock_close);
        write32(BIN_OFFSET + 0x0c022908, gdx_gethostbyname);
        write32(BIN_OFFSET + 0x0c022a04, gdx_connect_sock);
        write32(BIN_OFFSET + 0x0c04550c, gdx_connect_sock);
        write32(BIN_OFFSET + 0x0c033ec0, gdx_select);
        write32(BIN_OFFSET + 0x0c045818, gdx_select);
        write32(BIN_OFFSET + 0x0c1a9878, gdx_ppp_get_status);
        write32(BIN_OFFSET + 0x0c033fd8, gdx_lbs_sock_read);
        write32(BIN_OFFSET + 0x0c034464, gdx_lbs_sock_write);
        write32(BIN_OFFSET + 0x0c046678, gdx_mcs_sock_read);
        write32(BIN_OFFSET + 0x0c045820, gdx_mcs_sock_write);
        write32(BIN_OFFSET + 0x0c010a54, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c010e04, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c011038, gdx_softreset_disconnect);
        write32(BIN_OFFSET + 0x0c052134, gdx_softreset_disconnect);

#if !CALL_ORG_FUNC
        // skip ppp finalize
        for (u32 p = 0x0c022818; p <= 0x0c02282e; p += 2) {
            write16(BIN_OFFSET + p, 0x0009);
        }
#endif
    }
}

// replacement of internet_connect function
void GDXFUNC gdx_dial_start_disk1() {
    disk = 1;
    gdx_initialize();
    write8(0x0c2f6639, 2);
}

void GDXFUNC gdx_dial_start_disk2() {
    disk = 2;
    gdx_initialize();
#if CALL_ORG_FUNC
    // start dialing step
    write8(0x0c391d79, 1);
#else
    // skip dialing step
    write8(0x0c391d79, 2);
#endif
}

// The game calls these drawing hooks in its single-precision FPSCR context.
// As with the stats draw hooks below, use explicit float arithmetic so GCC
// does not insert ABI-based precision toggles around the operation.
static inline float gdx_add(float value, float offset) {
    __asm__("fadd %1,%0" : "+f"(value) : "f"(offset));
    return value;
}

struct gdx_ws_vtx {
    float x;
    float y;
    float z;
    u32 color;
};

// Independently patchable endpoints; the host overwrites them per aspect ratio.
GDXWSDATA float gdx_widescreen_transition_left_x = 0.0f;
GDXWSDATA float gdx_widescreen_transition_right_x = 640.0f;

// .ws follows the unchanged networking code at 0x0c4f0590. Keep the
// gdxsv-1.8.11 transition entry here; its C implementation follows the
// released HUD table and result hook below. The jump preserves PR.
asm(
    ".pushsection gdx.func.ws,\"ax\",@progbits\n"
    ".org 0\n"
    ".global gdx_widescreen_transition_matte\n"
    ".type gdx_widescreen_transition_matte, @function\n"
    "gdx_widescreen_transition_matte:\n"
    "\tmov.l\t.Ltransition_matte_impl,r0\n"
    "\tjmp\t@r0\n"
    "\t nop\n"
    "\t.balign 4\n"
    ".Ltransition_matte_impl:\n"
    "\t.long\tgdx_widescreen_transition_matte_impl\n"
    "\t.size gdx_widescreen_transition_matte, .-gdx_widescreen_transition_matte\n"
    ".popsection\n"
);

// FUN_0c049e20 rebuilds the general fade quad on every draw. Widen it at
// its private submit call, after the stock code sets XY and the live color.
void GDXWSFUNC gdx_widescreen_fade_submit(int count, struct gdx_ws_vtx *v) {
    float global_x = *(volatile float *)0x0c3d0594;
    float left = gdx_add(global_x, gdx_widescreen_transition_left_x);
    float right = gdx_add(global_x, gdx_widescreen_transition_right_x);
    v[0].x = v[1].x = left;
    v[2].x = v[3].x = right;
    ((void (*)(int, void *)) 0x0c19e630)(count, v);
}

// Battle HUD margin anchors; the host overwrites them per aspect ratio.
GDXWSDATA float gdx_widescreen_hud_right_offset = 0.0f;
GDXWSDATA float gdx_widescreen_hud_left_offset = 0.0f;
GDXWSDATA float gdx_widescreen_hud_half_left_offset = 0.0f;

// Installed for right-side types 0..3, left-side type 6 and information-panel
// type 12. Adds an aspect-derived displacement to the work field at +0x5c only
// while the stock renderer draws, then restores the original bits.
void GDXWSFUNC gdx_widescreen_hud_render_impl(void *work_) {
    u8 *work = (u8 *) work_;
    int type = *(signed char *)(work + 3);
    float offset;
    if (type == 12) {
        offset = gdx_widescreen_hud_half_left_offset;  // twice the screen response
    } else if (type == 6) {
        offset = gdx_widescreen_hud_left_offset;
    } else {
        offset = gdx_widescreen_hud_right_offset;
    }

    union { u32 bits; float value; } volatile *field = (void *)(work + 0x5c);
    u32 saved = field->bits;
    field->value = gdx_add(field->value, offset);

    // Dispatch through the untouched stock renderer table.
    u32 *stock_table = (u32 *) 0x0c2403a4;
    ((void (*)(void *)) stock_table[type])(work);

    // Never leak the widescreen displacement into the stock work state.
    field->bits = saved;
}

// Pad within .ws to the gdxsv-1.8.11 HUD callback at 0x0c4f0640 and its
// renderer table at 0x0c4f06b0. Saved objects retain the callback; creating
// new objects uses the table, so both must survive a payload replacement.
// .org rejects helpers which outgrow the gaps; ld.script checks the addresses.
// The jump changes neither PR nor the incoming work argument.
asm(
    ".pushsection gdx.func.ws,\"ax\",@progbits\n"
    ".org 0xb0\n"
    ".global gdx_widescreen_hud_render\n"
    ".type gdx_widescreen_hud_render, @function\n"
    "gdx_widescreen_hud_render:\n"
    "\tmov.l\t.Lhud_render_impl,r0\n"
    "\tjmp\t@r0\n"
    "\t nop\n"
    "\t.balign 4\n"
    ".Lhud_render_impl:\n"
    "\t.long\tgdx_widescreen_hud_render_impl\n"
    "\t.size gdx_widescreen_hud_render, .-gdx_widescreen_hud_render\n"
    ".org 0x120\n"
    ".global gdx_widescreen_hud_renderer_table\n"
    ".type gdx_widescreen_hud_renderer_table, @object\n"
    "gdx_widescreen_hud_renderer_table:\n"
    "\t.long\tgdx_widescreen_hud_render\n" // type 0: right
    "\t.long\tgdx_widescreen_hud_render\n" // type 1: right
    "\t.long\tgdx_widescreen_hud_render\n" // type 2: right
    "\t.long\tgdx_widescreen_hud_render\n" // type 3: right
    "\t.long\t0x0c11edea\n"
    "\t.long\t0x0c11ef96\n"
    "\t.long\tgdx_widescreen_hud_render\n" // type 6: left
    "\t.long\t0x0c11f940\n"
    "\t.long\t0x0c11fb18\n"
    "\t.long\t0x0c120268\n"
    "\t.long\t0x0c11bec0\n"
    "\t.long\t0x0c120380\n"
    "\t.long\tgdx_widescreen_hud_render\n" // type 12: information panel
    "\t.long\t0x0c121bc4\n"
    "\t.long\t0x0c121c74\n"
    "\t.long\t0x0c121c74\n"
    "\t.long\t0x0c121dbc\n"
    "\t.size gdx_widescreen_hud_renderer_table, .-gdx_widescreen_hud_renderer_table\n"
    ".popsection\n"
);

// This one hook must remain assembly: it enters FUN_0c1be120 mid-function
// with live fr5/r14 and replays the overwritten instructions. Keep its
// gdxsv-1.8.11 entry at 0x0c4f06f4 and its PC-relative literals together;
// resizing only changes the scale value.
asm(
    ".pushsection gdx.func.ws,\"ax\",@progbits\n"
    ".org 0x164\n"
    ".align 2\n"
    ".global gdx_widescreen_result_black_postproject\n"
    ".type gdx_widescreen_result_black_postproject, @function\n"
    "gdx_widescreen_result_black_postproject:\n"
    "	fmul	fr7,fr5\n"
    "	movt	r1\n"
    "	mov.l	r1,@-r15\n"
    "	sts	fpul,r1\n"
    "	mov.l	r1,@-r15\n"
    "	mov	r14,r1\n"
    "	add	#-8,r1\n"
    "	mov.l	@r1,r2\n"
    "	mov.l	.Lx_positive,r1\n"
    "	cmp/eq	r1,r2\n"
    "	bt	.Lcheck_y\n"
    "	mov.l	.Lx_negative,r1\n"
    "	cmp/eq	r1,r2\n"
    "	bf	.Lrestore_state\n"
    ".Lcheck_y:\n"
    "	mov	r14,r1\n"
    "	add	#-4,r1\n"
    "	mov.l	@r1,r2\n"
    "	mov.l	.Ly_positive,r1\n"
    "	cmp/eq	r1,r2\n"
    "	bt	.Lscale\n"
    "	mov.l	.Ly_negative,r1\n"
    "	cmp/eq	r1,r2\n"
    "	bf	.Lrestore_state\n"
    ".Lscale:\n"
    "	mov.l	gdx_widescreen_result_black_center,r2\n"
    "	lds	r2,fpul\n"
    "	fsts	fpul,fr0\n"
    "	fsub	fr0,fr5\n"
    "	mov.l	gdx_widescreen_result_black_scale,r2\n"
    "	lds	r2,fpul\n"
    "	fsts	fpul,fr1\n"
    "	fmul	fr1,fr5\n"
    "	fadd	fr0,fr5\n"
    ".Lrestore_state:\n"
    "	mov.l	@r15+,r1\n"
    "	lds	r1,fpul\n"
    "	mov.l	@r15+,r1\n"
    "	mov	#1,r2\n"
    "	cmp/eq	r2,r1\n"
    "	fmov.s	fr6,@-r6\n"
    "	add	#0x40,r5\n"
    "	fmov.s	fr4,@-r6\n"
    "	fmul	fr14,fr10\n"
    "	mov.l	r0,@r6\n"
    "	mov.l	.Lreturn,r2\n"
    "	jmp	@r2\n"
    "	 nop\n"
    "	.balign 4\n"
    ".Lx_positive:\n"
    "	.long	0x3e800001\n"
    ".Lx_negative:\n"
    "	.long	0xbe800001\n"
    ".Ly_positive:\n"
    "	.long	0x3e408313\n"
    ".Ly_negative:\n"
    "	.long	0xbe408313\n"
    ".Lreturn:\n"
    "	.long	0x0c1be1e8\n"
    "	.global gdx_widescreen_result_black_center\n"
    "	.type gdx_widescreen_result_black_center, @object\n"
    "gdx_widescreen_result_black_center:\n"
    "	.float	320.0\n"
    "	.size gdx_widescreen_result_black_center, 4\n"
    "	.global gdx_widescreen_result_black_scale\n"
    "	.type gdx_widescreen_result_black_scale, @object\n"
    "gdx_widescreen_result_black_scale:\n"
    "	.float	1.0\n"
    "	.size gdx_widescreen_result_black_scale, 4\n"
    "	.size gdx_widescreen_result_black_postproject, .-gdx_widescreen_result_black_postproject\n"
    ".popsection\n"
);

// Centred arbitrary-aspect replacement for FUN_0c1955b4. The released
// entry at 0x0c4f0590 jumps here, outside the fixed-address compatibility area.
void GDXWSFUNC gdx_widescreen_transition_matte_impl(void) {
    // Preserve the live packed color: (alpha << 24) | rgb.
    u32 rgb = read32(0x0c470428);
    u32 alpha = read32(0x0c47042c);
    u32 color = (alpha << 24) | rgb;

    float global_x = *(volatile float *)(0x0c3d0584 + 16);
    float global_y = *(volatile float *)(0x0c3d0584 + 20);
    float left = gdx_add(global_x, gdx_widescreen_transition_left_x);
    float right = gdx_add(global_x, gdx_widescreen_transition_right_x);
    float top = global_y;
    float bottom = gdx_add(global_y, 480.0f);

    struct gdx_ws_vtx v[4];
    v[0].x = left;  v[0].y = top;    v[0].z = 0.02f; v[0].color = color;
    v[1].x = left;  v[1].y = bottom; v[1].z = 0.02f; v[1].color = color;
    v[2].x = right; v[2].y = top;    v[2].z = 0.02f; v[2].color = color;
    v[3].x = right; v[3].y = bottom; v[3].z = 0.02f; v[3].color = color;

    ((void (*)(int)) 0x0c19e390)(0);             // prepare
    ((void (*)(int, void *)) 0x0c19e630)(4, v);  // submit(count, &vertices)
}

// These draw hooks run in the game's single-precision FPSCR context.
// Avoid GCC's ABI-based precision toggles around the multiplication.
static inline float gdx_stats_multiply(float value, float factor) {
    __asm__("fmul %1,%0" : "+f"(value) : "f"(factor));
    return value;
}

// Disc-2 record-line replacement. Other text calls retain the stock renderer.
struct gdx_player_info32_record {
    u32 valid;
    u32 battles;
    u32 wins;
    u32 losses;
    float scale_x;
    char text[40];
    u32 pad;
};

struct gdx_player_info32_record gdx_player_info32_records[4] GDXSTATSDATA = {0};

_Static_assert(sizeof(struct gdx_player_info32_record) == 64, "player-info32 saved layout");

void GDXSTATSFUNC gdx_player_info32_draw(const char *text, float x, float y, float z) {
    void (*draw)(const char *, float, float, float) = (void *)0x0c01e444;
    u32 caller = (u32)__builtin_return_address(0);
    caller &= 0x1fffffff;
    if (caller != 0x0c03e4c8 && caller != 0x0c03e644 &&
        caller != 0x0c03e7c0 && caller != 0x0c03e93c) {
        draw(text, x, y, z);
        return;
    }

    // Owner 0x0c03e2e0 keeps its advanced order cursor at incoming sp+0x10.
    const u8 *cursor = *(const u8 **)((u8 *)__builtin_dwarf_cfa() + 0x10);
    u32 player = cursor[-1];
    if (player >= 4 || !gdx_player_info32_records[player].valid) {
        draw(text, x, y, z);
        return;
    }

    struct gdx_player_info32_record *record = &gdx_player_info32_records[player];
    union { u32 bits; float value; } volatile *scale =
        (void *)(read32(0x0c391388) + 0x14b8);
    u32 saved = scale->bits;
    scale->value = gdx_stats_multiply(scale->value, record->scale_x);
    draw(record->text, x, y, z);
    scale->bits = saved;
}

// The largest displayed total is three UINT32_MAX counters (11 digits).
// Repeated subtraction avoids an SH4 64-bit division runtime dependency.
static u32 GDXSTATSFUNC gdx_stats_format_number(char *out, u32 capacity, u64 value, u32 width) {
    char digits[11];
    u32 length = 0;
    u64 power = 10000000000ULL;
    if (value >= 100000000000ULL)
        return 0;
    for (u32 i = 0; i < 11; ++i) {
        char digit = '0';
        while (value >= power) {
            value -= power;
            ++digit;
        }
        if (digit != '0' || length != 0 || i == 10)
            digits[length++] = digit;
        // Every remaining power fits in u32; constant division needs no helper.
        power = i == 0 ? 1000000000u : (u32)power / 10;
    }
    u32 size = length < width ? width : length;
    if (size >= capacity)
        return 0;
    u32 padding = size - length;
    for (u32 i = 0; i < padding; ++i)
        out[i] = ' ';
    for (u32 i = 0; i < length; ++i)
        out[padding + i] = digits[i];
    out[size] = 0;
    return size;
}

// Preserve the translated labels; only replace the three original %4d fields.
static u32 GDXSTATSFUNC gdx_stats_format_player(char *out, u32 capacity,
        const char *format, u32 format_size, const u32 values[3]) {
    u32 size = 0, field = 0;
    for (u32 i = 0; i < format_size && format[i] != 0;) {
        if (format[i] == '%') {
            if (i + 2 >= format_size || format[i + 1] != '4' ||
                    format[i + 2] != 'd' || field >= 3)
                return 0;
            u32 length = gdx_stats_format_number(out + size, capacity - size, values[field++], 4);
            if (length == 0)
                return 0;
            size += length;
            i += 3;
        } else {
            if (size + 1 >= capacity)
                return 0;
            out[size++] = format[i++];
        }
    }
    if (field != 3 || size >= capacity)
        return 0;
    out[size] = 0;
    return size;
}

static u32 GDXSTATSFUNC gdx_stats_read_be32(const u8 *data) {
    return (u32)data[0] << 24 | (u32)data[1] << 16 | (u32)data[2] << 8 | data[3];
}

static u16 GDXSTATSFUNC gdx_stats_read_be16(const u8 *data) {
    return (u16)data[0] << 8 | data[1];
}

static void GDXSTATSFUNC gdx_stats_write_be16(u8 *data, u16 value) {
    data[0] = value >> 8;
    data[1] = value;
}

// Encode min(1, width / length) as single-precision bits. Integer arithmetic
// avoids changing the game's FPSCR precision mode (which differs from GCC's).
static u32 GDXSTATSFUNC gdx_stats_scale_bits(u32 width, u32 length) {
    if (length <= width)
        return 0x3f800000;
    u32 exponent = 127, mantissa = 0;
    while (width < length) {
        width <<= 1;
        --exponent;
    }
    for (u32 i = 0; i < 24; ++i) {
        mantissa <<= 1;
        if (width >= length) {
            width -= length;
            mantissa |= 1;
        }
        width <<= 1;
    }
    // Round to nearest, ties to even.
    if (width > length || (width == length && (mantissa & 1)))
        ++mantissa;
    return ((exponent - 1) << 23) + mantissa;
}

// Validate the legacy prefix before accessing the appended 32-bit counters.
static u32 GDXSTATSFUNC gdx_stats_player_legacy_size(const u8 *body, u32 size) {
    if (size < 35 || size > 768 || body[0] < 1 || body[0] > 4)
        return 0;
    u32 legacy_size = size - 12;
    u32 cursor = 1;
    for (u32 field = 0; field < 3; ++field) {
        if (cursor + 2 > legacy_size)
            return 0;
        u32 length = (u32)body[cursor] << 8 | body[cursor + 1];
        cursor += 2;
        // The original receiver copies these fields without destination bounds.
        u32 capacity = field == 0 ? 7 : field == 1 ? 17 : 0x280;
        if (length > legacy_size - cursor || length > capacity)
            return 0;
        cursor += length;
    }
    return cursor + 16 == legacy_size ? legacy_size : 0;
}

// Personal ranking numbers only; labels and leaderboard text stay stock.
struct gdx_win_lose32_field {
    float scale_x;
    char text[16];
};

struct gdx_win_lose32_data {
    u32 valid;
    struct gdx_win_lose32_field fields[4];
} gdx_win_lose32_record GDXSTATSDATA = {0};

_Static_assert(sizeof(struct gdx_win_lose32_data) == 84, "win-lose32 saved layout");

void GDXSTATSFUNC gdx_win_lose32_draw(const char *text, float x, float y, float z) {
    void (*draw)(const char *, float, float, float) = (void *)0x0c01e444;
    u32 caller = (u32)__builtin_return_address(0) & 0x1fffffff;
    u32 field;
    if (caller == 0x0c04218c)
        field = 0;
    else if (caller == 0x0c0421c8)
        field = 1;
    else if (caller == 0x0c04222e)
        field = 2;
    else if (caller == 0x0c04226a)
        field = 3;
    else {
        draw(text, x, y, z);
        return;
    }
    // These draw sites serve every category. The cached 32-bit record is
    // only for category zero; top_rank_now selects the displayed category.
    if (!gdx_win_lose32_record.valid || read8(0x0c3a081b) != 0) {
        draw(text, x, y, z);
        return;
    }

    struct gdx_win_lose32_field *record = &gdx_win_lose32_record.fields[field];
    union { u32 bits; float value; } volatile *scale =
        (void *)(read32(0x0c391388) + 0x14b8);
    u32 saved = scale->bits;
    scale->value = gdx_stats_multiply(scale->value, record->scale_x);
    draw(record->text, x, y, z);
    scale->bits = saved;
}

// Disc-2 complete-message hooks. Framing and the original record receivers
// stay in the game; only the two private extensions are handled here.
struct gdx_stats_pending {
    u16 wire_seq;
    u16 original_seq;
    u16 command;
    u8 active;
};

static struct {
    u32 needs_init;
    u16 next_seq;
    struct gdx_stats_pending pending[5]; // four player cards, one personal panel
} gdx_stats_state GDXSTATSDATA = { .needs_init = 1 };

static void GDXSTATSFUNC gdx_stats_initialize() {
    if (!gdx_stats_state.needs_init)
        return;
    // Slot 99 is already connected. Do not call gdx_initialize(), which clears
    // is_online, or reset this state on an ordinary same-payload savestate load.
    for (u32 i = 0; i < 5; ++i)
        gdx_stats_state.pending[i].active = 0;
    gdx_stats_state.next_seq = 0;
    for (u32 i = 0; i < 4; ++i)
        gdx_player_info32_records[i].valid = 0;
    gdx_win_lose32_record.valid = 0;
    write32(BIN_OFFSET + 0x0c03e454, gdx_player_info32_draw);
    write32(BIN_OFFSET + 0x0c041f8c, gdx_win_lose32_draw);
    gdx_stats_state.needs_init = 0;
}

static void GDXSTATSFUNC gdx_stats_request(u16 command, u8 argument) {
    gdx_stats_initialize();
    if (command == 0x6145 && argument != 0) {
        // Other categories keep their legacy protocol and display. A late
        // personal-panel response must not complete the new category's request.
        gdx_stats_state.pending[4].active = 0;
        ((void (*)(int))0x0c034e9c)(argument);
        return;
    }
    if (command == 0x6913 && (argument < 1 || argument > 4)) {
        ((void (*)(int))0x0c036094)(argument);
        return;
    }

    u32 slot = command == 0x6913 ? argument - 1 : 4;
    struct gdx_stats_pending *pending = &gdx_stats_state.pending[slot];
    pending->active = 0;
    // Stock player requests are a burst sharing one sequence. Assign distinct
    // wire sequences so even empty error replies identify exactly one request.
    u16 seq;
    u32 collision;
    do {
        seq = ++gdx_stats_state.next_seq;
        collision = 0;
        for (u32 i = 0; i < 5; ++i)
            if (gdx_stats_state.pending[i].active && gdx_stats_state.pending[i].wire_seq == seq)
                collision = 1;
    } while (collision);

    u8 *tx = (void *)0x0c3a98c0;
    u16 original_seq = gdx_stats_read_be16(tx + 10);
    u16 extended = command == 0x6913 ? 0x9966 : 0x9967;
    ((void (*)(u8 *, u16))0x0c0341bc)(tx, extended);
    tx[5] = 1; // question
    ((void (*)(u8 *, u8))0x0c034288)(tx, argument);
    ((void (*)(u8 *))0x0c03426e)(tx);
    pending->wire_seq = seq;
    pending->original_seq = original_seq;
    pending->command = extended;
    pending->active = 1;
    if (slot < 4)
        gdx_player_info32_records[slot].valid = 0;
    else
        gdx_win_lose32_record.valid = 0;
    gdx_stats_write_be16(tx + 10, seq);
    ((void (*)(u8 *))0x0c03436c)(tx);
    gdx_stats_write_be16(tx + 10, original_seq);
}

void GDXSTATSFUNC gdx_player_info32_request(int player) {
    gdx_stats_request(0x6913, (u8)player);
}

void GDXSTATSFUNC gdx_win_lose32_request(int category) {
    gdx_stats_request(0x6145, (u8)category);
}

static u32 GDXSTATSFUNC gdx_stats_store_player(const u8 *body, u32 size, u32 slot) {
    u32 legacy_size = gdx_stats_player_legacy_size(body, size);
    if (!legacy_size || body[0] != slot + 1)
        return 0;
    struct gdx_player_info32_record *record = &gdx_player_info32_records[slot];
    u32 values[3];
    for (u32 i = 0; i < 3; ++i)
        values[i] = gdx_stats_read_be32(body + legacy_size + i * 4);
    record->battles = values[0];
    record->wins = values[1];
    record->losses = values[2];
    u32 length = gdx_stats_format_player(record->text, sizeof(record->text),
        (const char *)0x0c1d31bc, 16, values);
    if (length) {
        u32 scale = gdx_stats_scale_bits(18, length);
        __builtin_memcpy(&record->scale_x, &scale, sizeof(scale));
        record->valid = 1;
    }
    // An unrecognized translation can use the legacy text without stranding
    // an otherwise valid player-info reply before the battle starts.
    return legacy_size;
}

static u32 GDXSTATSFUNC gdx_stats_store_ranking(const u8 *body, u32 size) {
    if (size != 34)
        return 0;
    u64 wins = gdx_stats_read_be32(body + 18);
    u64 losses = gdx_stats_read_be32(body + 22);
    u64 draws = gdx_stats_read_be32(body + 26);
    u64 values[4] = {wins + losses + draws, wins, losses + draws, gdx_stats_read_be32(body + 30)};
    for (u32 i = 0; i < 4; ++i) {
        struct gdx_win_lose32_field *field = &gdx_win_lose32_record.fields[i];
        u32 length = gdx_stats_format_number(field->text, sizeof(field->text), values[i], 5);
        u32 scale = gdx_stats_scale_bits(5, length);
        __builtin_memcpy(&field->scale_x, &scale, sizeof(scale));
    }
    gdx_win_lose32_record.valid = 1;
    return 18;
}

int GDXSTATSFUNC gdx_stats_poll(u16 *command) {
    gdx_stats_initialize();
    int ready = ((int (*)(u16 *))0x0c033e20)(command);
    if (!ready || (*command != 0x9966 && *command != 0x9967))
        return ready;

    // The host exposes complete messages only, capped to this 768-byte body
    // buffer BEFORE the stock reader runs. The extension has now been drained.
    u8 *header = (void *)0x0c3a9bd0;
    u8 *body = header + 12;
    if (header[0] != 0x18 || header[1] != 2)
        return 0;
    u16 seq = gdx_stats_read_be16(header + 6);
    for (u32 slot = 0; slot < 5; ++slot) {
        struct gdx_stats_pending *pending = &gdx_stats_state.pending[slot];
        if (!pending->active || pending->wire_seq != seq || pending->command != *command)
            continue;
        pending->active = 0; // duplicates must never advance the completion count
        u32 legacy_size = 0;
        if (gdx_stats_read_be32(header + 8) == 0x00ffffff) {
            u32 size = gdx_stats_read_be16(header + 4);
            legacy_size = slot < 4 ? gdx_stats_store_player(body, size, slot)
                                  : gdx_stats_store_ranking(body, size);
        }
        if (!legacy_size) {
            // Both ranking callers ignore status and still run the receiver.
            // Clear the body as well, preserving the stock legacy error path.
            for (u32 i = 0; i < 768; ++i)
                body[i] = 0;
            for (u32 i = 8; i < 12; ++i)
                header[i] = 0xff;
        }
        *command = slot < 4 ? 0x6913 : 0x6145;
        gdx_stats_write_be16(header + 2, *command);
        gdx_stats_write_be16(header + 4, legacy_size);
        gdx_stats_write_be16(header + 6, pending->original_seq);
        return 1; // the original caller still owns receiver dispatch/completion
    }
    return 0; // consume unsolicited, superseded or duplicate extension replies
}
