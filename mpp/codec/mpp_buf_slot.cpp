/*
 * Copyright 2010 Rockchip Electronics S.LSI Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define  MODULE_TAG "mpp_buf_slot"

#include <string.h>

#include "mpp_log.h"
#include "mpp_mem.h"
#include "mpp_env.h"
#include "mpp_list.h"
#include "mpp_common.h"

#include "mpp_frame_impl.h"
#include "mpp_buf_slot.h"


#define BUF_SLOT_DBG_FUNCTION           (0x00000001)
#define BUF_SLOT_DBG_SETUP              (0x00000002)
#define BUF_SLOT_DBG_OPS_RUNTIME        (0x00000010)
#define BUF_SLOT_DBG_BUFFER             (0x00000100)
#define BUF_SLOT_DBG_FRAME              (0x00000200)
#define BUF_SLOT_DBG_OPS_HISTORY        (0x10000000)

#define buf_slot_dbg(flag, fmt, ...)    _mpp_dbg(buf_slot_debug, flag, fmt, ## __VA_ARGS__)

static RK_U32 buf_slot_debug = 0;

#define slot_assert(impl, cond) do {                                    \
    if (!(cond)) {                                                      \
        dump_slots(impl);                                               \
        mpp_err("Assertion %s failed at %s:%d\n",                       \
               MPP_STRINGS(cond), __FILE__, __LINE__);                  \
        abort();                                                        \
    }                                                                   \
} while (0)


#define MPP_SLOT_UNUSED                 (0x00000000)
#define MPP_SLOT_USED                   (0x00010000)
#define MPP_SLOT_NOT_FILLED             (0x00010000)
#define MPP_SLOT_USED_AS_DPB_REF        (0x00020000)
#define MPP_SLOT_USED_AS_DISPLAY        (0x00040000)
#define MPP_SLOT_USED_AS_HW_DST         (0x00080000)
#define MPP_SLOT_WITH_FRAME             (0x01000000)
#define MPP_SLOT_WITH_BUFFER            (0x02000000)
#define MPP_SLOT_HW_REF_MASK            (0x0000ffff)
#define MPP_SLOT_RELEASE_MASK           (0x00ffffff)

#define SLOT_CAN_RELEASE(slot)          ((slot->status&MPP_SLOT_RELEASE_MASK)==MPP_SLOT_UNUSED)

typedef struct MppBufSlotEntry_t {
    struct list_head    list;
    RK_U32              status;
    RK_S32              index;

    MppBuffer           buffer;
    MppFrame            frame;
} MppBufSlotEntry;

#define SLOT_OPS_MAX_COUNT              1024

typedef enum MppBufSlotOps_e {
    SLOT_INIT,
    SLOT_SET_NOT_READY,
    SLOT_CLR_NOT_READY,
    SLOT_SET_DPB_REF,
    SLOT_CLR_DPB_REF,
    SLOT_SET_DISPLAY,
    SLOT_CLR_DISPLAY,
    SLOT_SET_HW_DST,
    SLOT_CLR_HW_DST,
    SLOT_INC_HW_REF,
    SLOT_DEC_HW_REF,
    SLOT_SET_FRAME,
    SLOT_CLR_FRAME,
    SLOT_SET_BUFFER,
    SLOT_CLR_BUFFER,
} MppBufSlotOps;

static const char op_string[][16] = {
    "init         ",
    "set not ready",
    "set ready    ",
    "set dpb ref  ",
    "clr dpb ref  ",
    "set display  ",
    "clr display  ",
    "set hw dst   ",
    "clr hw dst   ",
    "inc hw ref   ",
    "dec hw ref   ",
    "set frame    ",
    "clr frame    ",
    "set buffer   ",
    "clr buffer   ",
};

typedef struct MppBufSlotLog_t {
    RK_U32              index;
    MppBufSlotOps       ops;
    RK_U32              status_in;
    RK_U32              status_out;
} MppBufSlotLog;

typedef struct MppBufSlotsImpl_t {
    Mutex               *lock;
    RK_U32              count;
    RK_U32              size;

    // status tracing
    RK_U32              decode_count;
    RK_U32              display_count;
    RK_U32              unrefer_count;

    // if slot changed, all will be hold until all slot is unused
    RK_U32              info_changed;
    RK_U32              new_count;
    RK_U32              new_size;

    // to record current output slot index
    RK_S32              output;

    // list for display
    struct list_head    display;

    // list for log
    mpp_list            *logs;

    MppBufSlotEntry     *slots;
} MppBufSlotsImpl;

static void add_slot_log(mpp_list *logs, RK_U32 index, MppBufSlotOps op, RK_U32 before, RK_U32 after)
{
    if (logs) {
        MppBufSlotLog log = {
            index,
            op,
            before,
            after,
        };
        if (logs->list_size() >= SLOT_OPS_MAX_COUNT)
            logs->del_at_head(NULL, sizeof(log));
        logs->add_at_tail(&log, sizeof(log));
    }
}

static void slot_ops_with_log(mpp_list *logs, MppBufSlotEntry *slot, MppBufSlotOps op)
{
    RK_U32 index  = slot->index;
    RK_U32 status = slot->status;
    RK_U32 before = status;
    switch (op) {
    case SLOT_INIT : {
        status = MPP_SLOT_UNUSED;
    } break;
    case SLOT_SET_NOT_READY : {
        status |= MPP_SLOT_USED;
    } break;
    case SLOT_CLR_NOT_READY : {
        status &= ~MPP_SLOT_USED;
    } break;
    case SLOT_SET_DPB_REF : {
        status |= MPP_SLOT_USED_AS_DPB_REF;
    } break;
    case SLOT_CLR_DPB_REF : {
        status &= ~MPP_SLOT_USED_AS_DPB_REF;
    } break;
    case SLOT_SET_DISPLAY : {
        status |= MPP_SLOT_USED_AS_DISPLAY;
    } break;
    case SLOT_CLR_DISPLAY : {
        status &= ~MPP_SLOT_USED_AS_DISPLAY;
    } break;
    case SLOT_SET_HW_DST : {
        status |= MPP_SLOT_USED_AS_HW_DST;
    } break;
    case SLOT_CLR_HW_DST : {
        status &= ~MPP_SLOT_USED_AS_HW_DST;
    } break;
    case SLOT_INC_HW_REF : {
        status++;
    } break;
    case SLOT_DEC_HW_REF : {
        status--;
    } break;
    case SLOT_SET_FRAME : {
        status |= MPP_SLOT_WITH_FRAME;
    } break;
    case SLOT_CLR_FRAME : {
        status &= ~MPP_SLOT_WITH_FRAME;
    } break;
    case SLOT_SET_BUFFER : {
        status |= MPP_SLOT_WITH_BUFFER;
    } break;
    case SLOT_CLR_BUFFER : {
        status &= ~MPP_SLOT_WITH_BUFFER;
    } break;
    default : {
        mpp_err("found invalid operation code %d\n", op);
    } break;
    }
    mpp_assert((RK_S16)(status & MPP_SLOT_HW_REF_MASK) >= 0);
    slot->status = status;
    buf_slot_dbg(BUF_SLOT_DBG_OPS_RUNTIME, "index %2d op: %s status in %08x out %08x",
                 index, op_string[op], before, status);
    add_slot_log(logs, index, op, before, status);
}

static void dump_slots(MppBufSlotsImpl *impl)
{
    RK_U32 i;
    MppBufSlotEntry *slot = impl->slots;

    mpp_log("\ndumping slots %p count %d size %d\n", impl, impl->count, impl->size);
    mpp_log("decode  count %d\n", impl->decode_count);
    mpp_log("display count %d\n", impl->display_count);

    for (i = 0; i < impl->count; i++, slot++) {
        RK_U32 used     = (slot->status & MPP_SLOT_USED)             ? (1) : (0);
        RK_U32 refer    = (slot->status & MPP_SLOT_USED_AS_DPB_REF)      ? (1) : (0);
        RK_U32 decoding = (slot->status & MPP_SLOT_USED_AS_HW_DST) ? (1) : (0);
        RK_U32 display  = (slot->status & MPP_SLOT_USED_AS_DISPLAY)  ? (1) : (0);

        mpp_log("slot %2d used %d refer %d decoding %d display %d\n",
                i, used, refer, decoding, display);
    }

    mpp_log("\nslot operation history:\n\n");

    mpp_list *logs = impl->logs;
    if (logs) {
        while (logs->list_size()) {
            MppBufSlotLog log;
            logs->del_at_head(&log, sizeof(log));
            mpp_log("index %2d op: %s status in %08x out %08x",
                    log.index, op_string[log.ops], log.status_in, log.status_out);
        }
    }

}

static void init_slot_entry(mpp_list *logs, MppBufSlotEntry *slot, RK_S32 pos, RK_S32 count)
{
    for (RK_S32 i = 0; i < count; i++, slot++) {
        INIT_LIST_HEAD(&slot->list);
        slot->index = pos + i;
        slot->frame = NULL;
        slot_ops_with_log(logs, slot, SLOT_INIT);
    }
}

/*
 * only called on unref / displayed / decoded
 *
 * NOTE: MppFrame will be destroyed outside mpp
 *       but MppBuffer must dec_ref here
 */
static void check_entry_unused(MppBufSlotsImpl *impl, MppBufSlotEntry *entry)
{
    if (SLOT_CAN_RELEASE(entry)) {
        if (entry->frame) {
            mpp_frame_deinit(&entry->frame);
            slot_ops_with_log(impl->logs, entry, SLOT_CLR_FRAME);
        } else
            mpp_buffer_put(entry->buffer);

        slot_ops_with_log(impl->logs, entry, SLOT_CLR_BUFFER);
    }
}

MPP_RET mpp_buf_slot_init(MppBufSlots *slots)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }
    MppBufSlotsImpl *impl = mpp_calloc(MppBufSlotsImpl, 1);
    if (NULL == impl) {
        *slots = NULL;
        return MPP_NOK;
    }

    mpp_env_get_u32("buf_slot_debug", &buf_slot_debug, 0);

    impl->lock = new Mutex();
    INIT_LIST_HEAD(&impl->display);

    if (buf_slot_debug & BUF_SLOT_DBG_OPS_HISTORY)
        impl->logs = new mpp_list(NULL);

    *slots = impl;
    return MPP_OK;
}

MPP_RET mpp_buf_slot_deinit(MppBufSlots slots)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    mpp_assert(list_empty(&impl->display));
    MppBufSlotEntry *slot = (MppBufSlotEntry *)impl->slots;
    RK_U32 i;
    for (i = 0; i < impl->count; i++, slot++)
        mpp_assert(MPP_SLOT_UNUSED == slot->status);

    if (impl->logs)
        delete impl->logs;

    delete impl->lock;
    mpp_free(impl->slots);
    mpp_free(slots);
    return MPP_OK;
}

MPP_RET mpp_buf_slot_setup(MppBufSlots slots, RK_U32 count, RK_U32 size, RK_U32 changed)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    buf_slot_dbg(BUF_SLOT_DBG_SETUP, "slot %p setup: count %d size %d changed %d\n",
                 slots, count, size, changed);

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    if (NULL == impl->slots) {
        // first slot setup
        impl->count = count;
        impl->size  = size;
        impl->slots = mpp_calloc(MppBufSlotEntry, count);
        init_slot_entry(impl->logs, impl->slots, 0, count);
    } else {
        // need to check info change or not
        if (!changed) {
            slot_assert(impl, size == impl->size);
            if (count > impl->count) {
                mpp_realloc(impl->slots, MppBufSlotEntry, count);
                init_slot_entry(impl->logs, impl->slots, impl->count, (count - impl->count));
            }
        } else {
            // info changed, even size is the same we still need to wait for new configuration
            impl->new_count     = count;
            impl->new_size      = size;
            impl->info_changed  = 1;
        }
    }

    return MPP_OK;
}

RK_U32 mpp_buf_slot_is_changed(MppBufSlots slots)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return 0;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    return impl->info_changed;
}

MPP_RET mpp_buf_slot_ready(MppBufSlots slots)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    buf_slot_dbg(BUF_SLOT_DBG_SETUP, "slot %p is ready now\n", slots);

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, impl->info_changed);
    slot_assert(impl, impl->slots);

    impl->info_changed  = 0;
    impl->size          = impl->new_size;
    if (impl->count != impl->new_count) {
        mpp_realloc(impl->slots, MppBufSlotEntry, impl->new_count);
        init_slot_entry(impl->logs, impl->slots, 0, impl->new_count);
    }
    impl->count = impl->new_count;
    if (impl->logs) {
        mpp_list *logs = impl->logs;
        while (logs->list_size())
            logs->del_at_head(NULL, sizeof(MppBufSlotLog));
    }
    return MPP_OK;
}

RK_U32  mpp_buf_slot_get_size(MppBufSlots slots)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return 0;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    return impl->size;
}

MPP_RET mpp_buf_slot_get_unused(MppBufSlots slots, RK_U32 *index)
{
    if (NULL == slots || NULL == index) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    RK_U32 i;
    MppBufSlotEntry *slot = impl->slots;
    for (i = 0; i < impl->count; i++, slot++) {
        if (MPP_SLOT_UNUSED == slot->status) {
            *index = i;
            slot_ops_with_log(impl->logs, slot, SLOT_SET_NOT_READY);
            return MPP_OK;
        }
    }

    *index = -1;
    mpp_err_f("failed to get a unused slot\n");
    dump_slots(impl);
    slot_assert(impl, 0);
    return MPP_NOK;
}

MPP_RET mpp_buf_slot_set_dpb_ref(MppBufSlots slots, RK_U32 index)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, index < impl->count);
    slot_ops_with_log(impl->logs, &impl->slots[index], SLOT_SET_DPB_REF);
    return MPP_OK;
}

MPP_RET mpp_buf_slot_clr_dpb_ref(MppBufSlots slots, RK_U32 index)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, index < impl->count);
    MppBufSlotEntry *slot = &impl->slots[index];
    slot_ops_with_log(impl->logs, slot, SLOT_CLR_DPB_REF);
    check_entry_unused(impl, slot);
    return MPP_OK;
}

MPP_RET mpp_buf_slot_set_display(MppBufSlots slots, RK_U32 index)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, index < impl->count);
    MppBufSlotEntry *slot = &impl->slots[index];
    slot_ops_with_log(impl->logs, slot, SLOT_SET_DISPLAY);

    // add slot to display list
    list_del_init(&slot->list);
    list_add_tail(&slot->list, &impl->display);
    return MPP_OK;
}

MPP_RET mpp_buf_slot_set_hw_dst(MppBufSlots slots, RK_U32 index, MppFrame frame)
{
    if (NULL == slots || NULL == frame) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, index < impl->count);
    MppBufSlotEntry *slot = &impl->slots[index];
    slot_ops_with_log(impl->logs, slot, SLOT_SET_HW_DST);

    slot_assert(impl, slot->status & MPP_SLOT_NOT_FILLED);
    if (NULL == slot->frame)
        mpp_frame_init(&slot->frame);

    mpp_frame_copy(slot->frame, frame);
    slot_ops_with_log(impl->logs, slot, SLOT_SET_FRAME);
    impl->output = index;
    return MPP_OK;
}

MPP_RET mpp_buf_slot_clr_hw_dst(MppBufSlots slots, RK_U32 index)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, index < impl->count);
    MppBufSlotEntry *slot = &impl->slots[index];
    slot_ops_with_log(impl->logs, slot, SLOT_CLR_HW_DST);
    slot_ops_with_log(impl->logs, slot, SLOT_CLR_NOT_READY);
    impl->decode_count++;
    check_entry_unused(impl, slot);
    return MPP_OK;
}

MPP_RET mpp_buf_slot_get_hw_dst(MppBufSlots slots, RK_U32 *index)
{
    if (NULL == slots || NULL == index) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    *index = impl->output;
    return MPP_OK;
}

MPP_RET mpp_buf_slot_inc_hw_ref(MppBufSlots slots, RK_U32 index)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, index < impl->count);
    MppBufSlotEntry *slot = &impl->slots[index];
    slot_ops_with_log(impl->logs, slot, SLOT_INC_HW_REF);
    return MPP_OK;
}

MPP_RET mpp_buf_slot_dec_hw_ref(MppBufSlots slots, RK_U32 index)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, index < impl->count);
    MppBufSlotEntry *slot = &impl->slots[index];
    slot_ops_with_log(impl->logs, slot, SLOT_DEC_HW_REF);
    check_entry_unused(impl, slot);
    return MPP_OK;
}

MPP_RET mpp_buf_slot_set_buffer(MppBufSlots slots, RK_U32 index, MppBuffer buffer)
{
    if (NULL == slots || NULL == buffer) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, index < impl->count);
    MppBufSlotEntry *slot = &impl->slots[index];
    if (slot->buffer) {
        // NOTE: reset buffer only on stream buffer slot
        mpp_assert(NULL == slot->frame);
        mpp_buffer_put(slot->buffer);
    }
    slot->buffer = buffer;
    if (slot->frame)
        mpp_frame_set_buffer(slot->frame, buffer);

    mpp_buffer_inc_ref(buffer);
    slot_ops_with_log(impl->logs, slot, SLOT_SET_BUFFER);
    return MPP_OK;
}

MppBuffer mpp_buf_slot_get_buffer(MppBufSlots slots, RK_U32 index)
{
    if (NULL == slots) {
        mpp_err_f("found NULL input\n");
        return NULL;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    slot_assert(impl, index < impl->count);
    MppBufSlotEntry *slot = &impl->slots[index];
    return slot->buffer;
}

MPP_RET mpp_buf_slot_get_display(MppBufSlots slots, MppFrame *frame)
{
    if (NULL == slots || NULL == frame) {
        mpp_err_f("found NULL input\n");
        return MPP_ERR_NULL_PTR;
    }

    MppBufSlotsImpl *impl = (MppBufSlotsImpl *)slots;
    Mutex::Autolock auto_lock(impl->lock);
    if (list_empty(&impl->display))
        return MPP_NOK;

    MppBufSlotEntry *slot = list_entry(impl->display.next, MppBufSlotEntry, list);
    if (slot->status & MPP_SLOT_NOT_FILLED)
        return MPP_NOK;

    // make sure that this slot is just the next display slot
    list_del_init(&slot->list);

    MppFrame display;
    mpp_frame_init(&display);
    mpp_frame_copy(display, slot->frame);
    *frame = display;

    RK_U32 index = slot->index;
    slot_assert(impl, index < impl->count);
    slot_ops_with_log(impl->logs, slot, SLOT_CLR_DISPLAY);
    check_entry_unused(impl, slot);
    impl->display_count++;
    return MPP_OK;
}


