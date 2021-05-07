/* Copyright (c) 2019-2021 Advanced Micro Devices, Inc.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

#include "wave.h"
#include "architecture.h"
#include "dispatch.h"
#include "displaced_stepping.h"
#include "event.h"
#include "initialization.h"
#include "logging.h"
#include "memory.h"
#include "os_driver.h"
#include "process.h"
#include "queue.h"
#include "register.h"
#include "utils.h"
#include "watchpoint.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace amd::dbgapi
{

wave_t::wave_t (amd_dbgapi_wave_id_t wave_id, dispatch_t &dispatch,
                const callbacks_t &callbacks)
  : handle_object (wave_id),
    m_register_cache (dispatch.process (),
                      memory_cache_t::policy_t::write_back),
    m_callbacks (callbacks), m_dispatch (dispatch)
{
}

wave_t::~wave_t ()
{
  if (displaced_stepping ())
    {
      /* displaced step operations are cancelled by the process on detach,
         unless the process has exited and the queue is invalid, in which case,
         we simply release the displaced stepping buffer.  */
      dbgapi_assert (!queue ().is_valid ());
      displaced_stepping_t::release (m_displaced_stepping);
    }
}

void
wave_t::set_visibility (visibility_t visibility)
{
  if (m_visibility == visibility)
    return;

  m_visibility = visibility;

  /* Since the visibility of this wave has changed, the list of waves returned
     by the process has also changed.  */
  process ().set_changed<wave_t> (true);
}

uint64_t
wave_t::exec_mask () const
{

  if (lane_count () == 32)
    {
      uint32_t exec;
      read_register (amdgpu_regnum_t::exec_32, &exec);
      return exec;
    }
  else if (lane_count () == 64)
    {
      uint64_t exec;
      read_register (amdgpu_regnum_t::exec_64, &exec);
      return exec;
    }
  error ("Not a valid lane_count for EXEC mask: %zu", lane_count ());
}

amd_dbgapi_global_address_t
wave_t::pc () const
{
  amd_dbgapi_global_address_t pc;
  read_register (amdgpu_regnum_t::pc, &pc);
  return pc;
}

std::optional<std::vector<uint8_t>>
wave_t::instruction_at_pc () const
{
  size_t size = architecture ().largest_instruction_size ();
  std::vector<uint8_t> instruction_bytes (size);

  amd_dbgapi_status_t status = process ().read_global_memory_partial (
    pc (), instruction_bytes.data (), &size);
  if (status != AMD_DBGAPI_STATUS_SUCCESS)
    return {};

  /* Trim unread bytes.  */
  instruction_bytes.resize (size);

  return instruction_bytes;
}

void
wave_t::park ()
{
  dbgapi_assert (state () == AMD_DBGAPI_WAVE_STATE_STOP
                 && "Cannot park a running wave");

  dbgapi_assert (!m_is_parked && "already parked");

  /* On architectures that do not support halting at an s_endpgm instruction,
     when a wave is stopped, we change its pc to point to an immutable trap
     instruction.  This guarantees that the wave will never be halted at an
     s_endpgm.  */
  m_parked_pc = pc ();

  amd_dbgapi_global_address_t parked_pc
    = m_callbacks.park_instruction_address ();
  write_register (amdgpu_regnum_t::pc, &parked_pc);

  m_is_parked = true;
  /* From now on, every read/write to the pc register will be from/to
     m_parked_pc.  The real pc in the context save area will be untouched.  */

  dbgapi_log (AMD_DBGAPI_LOG_LEVEL_VERBOSE, "parked %s (pc=%#lx)",
              to_string (id ()).c_str (), pc ());
}

void
wave_t::unpark ()
{
  dbgapi_assert (state () != AMD_DBGAPI_WAVE_STATE_STOP
                 && "Cannot unpark a stopped wave");

  dbgapi_assert (m_is_parked && "not parked");

  amd_dbgapi_global_address_t saved_pc = pc ();

  m_is_parked = false;
  /* From now on, every read/write to the pc register will be from/to the
     context save area.  */

  write_register (amdgpu_regnum_t::pc, &saved_pc);

  dbgapi_log (AMD_DBGAPI_LOG_LEVEL_VERBOSE, "unparked %s (pc=%#lx)",
              to_string (id ()).c_str (), pc ());
}

void
wave_t::terminate ()
{
  if (m_displaced_stepping)
    {
      displaced_stepping_t::release (m_displaced_stepping);
      m_displaced_stepping = nullptr;
    }

  /* Mark the wave as invalid and un-halt it at an s_endpgm instruction. This
     allows the hardware to terminate the wave, while ensuring that the wave is
     never reported to the client as existing.  */

  amd_dbgapi_global_address_t terminate_pc
    = m_callbacks.endpgm_instruction_address ();

  /* Make the PC point to an immutable s_endpgm instruction.  */
  write_register (amdgpu_regnum_t::pc, &terminate_pc);

  /* Hide this wave so that it isn't reported to the client.  */
  set_visibility (wave_t::visibility_t::hidden_at_endpgm);

  set_state (AMD_DBGAPI_WAVE_STATE_RUN);
}

void
wave_t::displaced_stepping_start (const void *saved_instruction_bytes)
{
  dbgapi_assert (!m_displaced_stepping && "already displaced stepping");
  dbgapi_assert (state () == AMD_DBGAPI_WAVE_STATE_STOP && "not stopped");

  /* Check if we already have a displaced stepping buffer for this pc
     that can be shared between waves associated with the same queue.
   */
  displaced_stepping_t *displaced_stepping = process ().find_if (
    [&] (const displaced_stepping_t &other)
    { return other.queue () == queue () && other.from () == pc (); });

  if (displaced_stepping)
    {
      displaced_stepping_t::retain (displaced_stepping);
    }
  else
    {
      /* If we can't share a displaced stepping operation with another
         wave, create a new one.  */

      /* Reconstitute the original instruction bytes.  */
      std::vector<uint8_t> original_instruction (
        architecture ().largest_instruction_size ());

      memcpy (original_instruction.data (), saved_instruction_bytes,
              architecture ().breakpoint_instruction ().size ());

      size_t offset = architecture ().breakpoint_instruction ().size ();
      size_t remaining_size = original_instruction.size () - offset;

      amd_dbgapi_status_t status = process ().read_global_memory_partial (
        pc () + offset, original_instruction.data () + offset,
        &remaining_size);
      if (status != AMD_DBGAPI_STATUS_SUCCESS)
        throw exception_t (status);

      /* Trim unread bytes.  */
      original_instruction.resize (offset + remaining_size);

      /* Trim to size of instruction.  */
      size_t instruction_size
        = architecture ().instruction_size (original_instruction);

      if (!instruction_size)
        {
          /* If instruction_size is 0, the disassembler did not recognize the
             instruction.  This instruction may be non-sequencial, and we won't
             be able to tell if the jump is relative or absolute.  */
          throw exception_t (AMD_DBGAPI_STATUS_ERROR_ILLEGAL_INSTRUCTION);
        }
      original_instruction.resize (instruction_size);

      bool simulate = architecture ().can_simulate (original_instruction);

      if (!architecture ().can_execute_displaced (original_instruction)
          && !simulate)
        {
          /* If this instruction cannot be displaced-stepped nor simulated,
             then it must be inline-stepped.  */
          throw exception_t (AMD_DBGAPI_STATUS_ERROR_ILLEGAL_INSTRUCTION);
        }

      instruction_buffer_t instruction_buffer{};

      if (!simulate)
        {
          instruction_buffer = m_callbacks.allocate_instruction_buffer ();
          instruction_buffer->resize (original_instruction.size ());
          amd_dbgapi_global_address_t instruction_addr
            = instruction_buffer->begin ();

          if (process ().write_global_memory (instruction_addr,
                                              original_instruction.data (),
                                              original_instruction.size ())
              != AMD_DBGAPI_STATUS_SUCCESS)
            error ("Could not write the displaced instruction");
        }

      displaced_stepping = &process ().create<displaced_stepping_t> (
        queue (), pc (), std::move (original_instruction), simulate,
        std::move (instruction_buffer));
    }

  if (!displaced_stepping->is_simulated ())
    {
      amd_dbgapi_global_address_t displaced_pc = displaced_stepping->to ();
      dbgapi_assert (displaced_pc != amd_dbgapi_global_address_t{});

      write_register (amdgpu_regnum_t::pc, &displaced_pc);

      dbgapi_log (AMD_DBGAPI_LOG_LEVEL_INFO,
                  "changing %s's pc from %#lx to %#lx (started %s)",
                  to_string (id ()).c_str (), displaced_stepping->from (),
                  displaced_stepping->to (),
                  to_string (displaced_stepping->id ()).c_str ());
    }

  m_displaced_stepping = displaced_stepping;
}

void
wave_t::displaced_stepping_complete ()
{
  dbgapi_assert (!!m_displaced_stepping && "not displaced stepping");
  dbgapi_assert (state () == AMD_DBGAPI_WAVE_STATE_STOP && "not stopped");

  if (!m_displaced_stepping->is_simulated ())
    {
      amd_dbgapi_global_address_t displaced_pc = pc ();
      amd_dbgapi_global_address_t restored_pc = displaced_pc
                                                + m_displaced_stepping->from ()
                                                - m_displaced_stepping->to ();
      write_register (amdgpu_regnum_t::pc, &restored_pc);

      dbgapi_log (AMD_DBGAPI_LOG_LEVEL_INFO,
                  "changing %s's pc from %#lx to %#lx (%s %s)",
                  to_string (id ()).c_str (), displaced_pc, pc (),
                  displaced_pc == m_displaced_stepping->to () ? "aborted"
                                                              : "completed",
                  to_string (m_displaced_stepping->id ()).c_str ());
    }

  displaced_stepping_t::release (m_displaced_stepping);
  m_displaced_stepping = nullptr;
}

void
wave_t::update (const wave_t &group_leader,
                std::unique_ptr<architecture_t::cwsr_record_t> cwsr_record)
{
  dbgapi_assert (queue ().is_suspended ());
  const bool first_update = !m_cwsr_record;

  m_cwsr_record = std::move (cwsr_record);
  m_group_leader = &group_leader;

  constexpr auto first_cached_register = amdgpu_regnum_t::first_hwreg;
  constexpr auto last_cached_register = amdgpu_regnum_t::last_ttmp;

  auto register_cache_begin = register_address (first_cached_register);
  dbgapi_assert (register_cache_begin);

  /* Update the wave's state if this is a new wave, or if the wave was running
     the last time the queue it belongs to was resumed.  */
  if (m_state != AMD_DBGAPI_WAVE_STATE_STOP)
    {
      auto last_cached_register_address
        = register_address (last_cached_register);
      auto last_cached_register_size
        = architecture ().register_size (last_cached_register);
      dbgapi_assert (last_cached_register_address
                     && last_cached_register_size);

      amd_dbgapi_global_address_t register_cache_end
        = *last_cached_register_address + *last_cached_register_size;
      dbgapi_assert (register_cache_end > *register_cache_begin);

      /* Since the wave was previously running, the content of the cached
         registers may have changed.  */
      m_register_cache.reset (*register_cache_begin,
                              register_cache_end - *register_cache_begin);

      architecture ().get_wave_state (*this, &m_state, &m_stop_reason);

      /* The wave was running, and it is now stopped. Park it.  */
      if (m_state == AMD_DBGAPI_WAVE_STATE_STOP
          && !architecture ().can_halt_at_endpgm ())
        park ();

      if (visibility () == visibility_t::visible
          && m_state == AMD_DBGAPI_WAVE_STATE_STOP
          && m_stop_reason != AMD_DBGAPI_WAVE_STOP_REASON_NONE)
        raise_event (AMD_DBGAPI_EVENT_KIND_WAVE_STOP);
    }
  else
    {
      /* The address of this cwsr_record may have changed since the last
       context save, relocate the hwregs cache.  */
      m_register_cache.relocate (*register_cache_begin);
    }

  /* If this is the first time we update this wave, store the wave_id, and load
     the immutable state from the ttmp registers (group_ids, wave_in_group,
     scratch_offset).  */
  if (first_update)
    {
      /* Write the wave_id register.  */
      amd_dbgapi_wave_id_t wave_id = id ();
      write_register (amdgpu_regnum_t::wave_id, &wave_id);

      /* Read group_ids[0:3].  */
      read_register (amdgpu_regnum_t::dispatch_grid, 0, sizeof (m_group_ids),
                     &m_group_ids[0]);

      /* Read the wave's position in the thread group.  */
      read_register (amdgpu_regnum_t::wave_in_group, &m_wave_in_group);

      if (dispatch ().is_scratch_enabled ())
        {
          uint32_t scratch_offset;
          read_register (amdgpu_regnum_t::scratch_offset, &scratch_offset);
          m_scratch_offset = scratch_offset;
        }
    }
}

void
wave_t::set_state (amd_dbgapi_wave_state_t state)
{
  amd_dbgapi_wave_state_t prev_state = m_state;

  if (state == prev_state)
    return;

  dbgapi_assert (
    (!m_displaced_stepping || state != AMD_DBGAPI_WAVE_STATE_RUN)
    && "displaced-stepping waves can only be stopped or single-stepped");

  dbgapi_assert ((state == AMD_DBGAPI_WAVE_STATE_STOP
                  || !(m_stop_reason & ~resumable_stop_reason_mask))
                 && "cannot resume a wave with fatal exceptions");

  m_stop_requested = state == AMD_DBGAPI_WAVE_STATE_STOP;

  /* A wave single-stepping an s_endpgm instruction does not generate a trap
     exception upon executing the instruction, so we need to immediately
     terminate the wave and enqueue an aborted command event.  */
  if (state == AMD_DBGAPI_WAVE_STATE_SINGLE_STEP)
    {
      auto instruction = instruction_at_pc ();
      bool is_endpgm
        = /* the simulated displaced instruction is s_endpgm */ (
            m_displaced_stepping && m_displaced_stepping->is_simulated ()
            && architecture ().is_endpgm (
              m_displaced_stepping->original_instruction ()))
          || /* the current instruction at pc is s_endpgm  */ (
            instruction && architecture ().is_endpgm (*instruction));

      if (is_endpgm)
        {
          terminate ();
          raise_event (AMD_DBGAPI_EVENT_KIND_WAVE_COMMAND_TERMINATED);
          return;
        }
    }

  if (visibility () == visibility_t::visible)
    dbgapi_log (AMD_DBGAPI_LOG_LEVEL_INFO,
                "changing %s's state from %s to %s (pc=%#lx)",
                to_string (id ()).c_str (), to_string (prev_state).c_str (),
                to_string (state).c_str (), pc ());

  /* Single-stepping a simulated displaced instruction does not require the
     wave to run, simulate resuming of the wave as well.  */
  if (state == AMD_DBGAPI_WAVE_STATE_SINGLE_STEP && m_displaced_stepping
      && m_displaced_stepping->is_simulated ())
    {
      if (architecture ().simulate_instruction (
            *this, m_displaced_stepping->from (),
            m_displaced_stepping->original_instruction ()))
        {
          m_state = AMD_DBGAPI_WAVE_STATE_STOP;
          m_stop_reason = AMD_DBGAPI_WAVE_STOP_REASON_SINGLE_STEP;

          raise_event (AMD_DBGAPI_EVENT_KIND_WAVE_STOP);
          return;
        }
    }

  architecture ().set_wave_state (*this, state);
  m_state = state;

  if (!architecture ().can_halt_at_endpgm ())
    {
      if (state == AMD_DBGAPI_WAVE_STATE_STOP)
        park ();
      else
        unpark ();
    }

  if (state != AMD_DBGAPI_WAVE_STATE_STOP)
    {
      dbgapi_assert (prev_state == AMD_DBGAPI_WAVE_STATE_STOP
                     && "cannot resume an already running wave");

      /* m_last_stopped_pc is used to detect spurious single-step events
         (entered the trap handler with mode.debug_en=1 but pc ==
         m_last_stopped_pc).  Save the pc here as this is the last known
         pc before the wave is unhalted.  */
      m_last_stopped_pc = pc ();

      /* Clear the stop reason.  */
      m_stop_reason = AMD_DBGAPI_WAVE_STOP_REASON_NONE;
    }
  else if (prev_state != AMD_DBGAPI_WAVE_STATE_STOP)
    {
      /* We requested the wave be stopped, and the wave wasn't already stopped,
         report an event to acknowledge that the wave has stopped.  */

      m_stop_reason = AMD_DBGAPI_WAVE_STOP_REASON_NONE;

      dbgapi_assert (visibility () == visibility_t::visible
                     && "cannot request a hidden wave to stop");

      raise_event (prev_state == AMD_DBGAPI_WAVE_STATE_SINGLE_STEP
                     ? AMD_DBGAPI_EVENT_KIND_WAVE_COMMAND_TERMINATED
                     : AMD_DBGAPI_EVENT_KIND_WAVE_STOP);
    }
}

memory_cache_t::policy_t
wave_t::register_cache_policy (amdgpu_regnum_t regnum) const
{
  dbgapi_assert (!is_pseudo_register (regnum)
                 && "pseudo registers do not have a cache policy");

  auto reg_addr = register_address (regnum);
  auto reg_size = architecture ().register_size (regnum);
  dbgapi_assert (reg_addr && reg_size && "invalid register");

  if (m_register_cache.contains (*reg_addr, *reg_size))
    return m_register_cache.policy ();

  return memory_cache_t::policy_t::uncached;
}

bool
wave_t::is_register_available (amdgpu_regnum_t regnum) const
{
  if (is_pseudo_register (regnum))
    return architecture ().is_pseudo_register_available (*this, regnum);

  return register_address (regnum).has_value ();
}

void
wave_t::read_register (amdgpu_regnum_t regnum, size_t offset,
                       size_t value_size, void *value) const
{
  if (is_pseudo_register (regnum))
    return architecture ().read_pseudo_register (*this, regnum, offset,
                                                 value_size, value);

  auto reg_addr = register_address (regnum);
  auto reg_size = architecture ().register_size (regnum);

  if (!reg_addr || !reg_size)
    throw exception_t (AMD_DBGAPI_STATUS_ERROR_INVALID_REGISTER_ID);

  if (!value_size || (offset + value_size) > *reg_size)
    throw exception_t (AMD_DBGAPI_STATUS_ERROR_INVALID_ARGUMENT_COMPATIBILITY);

  if (m_is_parked && regnum == amdgpu_regnum_t::pc)
    {
      memcpy (static_cast<char *> (value) + offset,
              reinterpret_cast<const char *> (&m_parked_pc) + offset,
              value_size);
      return;
    }

  /* hwregs are cached, so return the value from the cache.  */
  if (m_register_cache.contains (*reg_addr + offset, value_size))
    {
      if (m_register_cache.read (*reg_addr + offset,
                                 static_cast<char *> (value) + offset,
                                 value_size)
          != AMD_DBGAPI_STATUS_SUCCESS)
        error ("Could not read '%s' from the register cache",
               architecture ().register_name (regnum)->c_str ());
    }
  else
    {
      dbgapi_assert (queue ().is_suspended ());

      if (process ().read_global_memory (*reg_addr + offset,
                                         static_cast<char *> (value) + offset,
                                         value_size)
          != AMD_DBGAPI_STATUS_SUCCESS)
        error ("Could not read the '%s' register",
               architecture ().register_name (regnum)->c_str ());
    }
}

void
wave_t::write_register (amdgpu_regnum_t regnum, size_t offset,
                        size_t value_size, const void *value)
{
  if (is_pseudo_register (regnum))
    return architecture ().write_pseudo_register (*this, regnum, offset,
                                                  value_size, value);

  auto reg_addr = register_address (regnum);
  auto reg_size = architecture ().register_size (regnum);

  if (!reg_addr || !reg_size)
    throw exception_t (AMD_DBGAPI_STATUS_ERROR_INVALID_REGISTER_ID);

  if (!value_size || (offset + value_size) > *reg_size)
    throw exception_t (AMD_DBGAPI_STATUS_ERROR_INVALID_ARGUMENT_COMPATIBILITY);

  if (m_is_parked && regnum == amdgpu_regnum_t::pc)
    {
      memcpy (reinterpret_cast<char *> (&m_parked_pc) + offset,
              static_cast<const char *> (value) + offset, value_size);
      return;
    }

  if (m_register_cache.contains (*reg_addr + offset, value_size))
    {
      if (m_register_cache.write (*reg_addr + offset,
                                  static_cast<const char *> (value) + offset,
                                  value_size)
          != AMD_DBGAPI_STATUS_SUCCESS)
        error ("Could not write '%s' to the register cache",
               architecture ().register_name (regnum)->c_str ());

      /* If the cache is dirty, register it with the queue, it will be flushed
         when the queue is resumed.  */
      if (m_register_cache.is_dirty ())
        m_callbacks.register_dirty_cache (m_register_cache);
    }
  else
    {
      dbgapi_assert (queue ().is_suspended ());

      if (process ().write_global_memory (
            *reg_addr + offset, static_cast<const char *> (value) + offset,
            value_size)
          != AMD_DBGAPI_STATUS_SUCCESS)
        error ("Could not write the '%s' register",
               architecture ().register_name (regnum)->c_str ());
    }
}

amd_dbgapi_status_t
wave_t::xfer_private_memory_swizzled (
  amd_dbgapi_segment_address_t segment_address, amd_dbgapi_lane_id_t lane_id,
  void *read, const void *write, size_t *size)
{
  if (lane_id == AMD_DBGAPI_LANE_NONE || lane_id >= lane_count ())
    return AMD_DBGAPI_STATUS_ERROR_INVALID_LANE_ID;

  if (!dispatch ().is_scratch_enabled ())
    return AMD_DBGAPI_STATUS_ERROR_MEMORY_ACCESS;

  amd_dbgapi_size_t limit = m_callbacks.scratch_memory_size ();
  amd_dbgapi_global_address_t scratch_base
    = m_callbacks.scratch_memory_base ();

  size_t bytes = *size;
  while (bytes > 0)
    {
      /* Transfer one aligned dword at a time, except for the first (or last)
         read which could read less than a dword if the start (or end) address
         is not aligned.  */

      size_t request_size = std::min (4 - (segment_address % 4), bytes);
      size_t xfer_size = request_size;

      amd_dbgapi_size_t offset = m_scratch_offset
                                 + ((segment_address / 4) * lane_count () * 4)
                                 + (lane_id * 4) + (segment_address % 4);

      if ((offset + xfer_size) > limit)
        {
          xfer_size = offset < limit ? limit - offset : 0;
          if (xfer_size == 0)
            return AMD_DBGAPI_STATUS_ERROR_MEMORY_ACCESS;
        }

      amd_dbgapi_global_address_t global_address = scratch_base + offset;

      amd_dbgapi_status_t status;
      if (read)
        status = process ().read_global_memory_partial (global_address, read,
                                                        &xfer_size);
      else
        status = process ().write_global_memory_partial (global_address, write,
                                                         &xfer_size);
      if (status != AMD_DBGAPI_STATUS_SUCCESS)
        return status;

      bytes -= xfer_size;
      if (request_size != xfer_size)
        break;

      if (read)
        read = static_cast<char *> (read) + xfer_size;
      else
        write = static_cast<const char *> (write) + xfer_size;

      segment_address += xfer_size;
    }

  if (bytes && bytes == *size)
    return AMD_DBGAPI_STATUS_ERROR_MEMORY_ACCESS;

  *size -= bytes;
  return AMD_DBGAPI_STATUS_SUCCESS;
}

amd_dbgapi_status_t
wave_t::xfer_private_memory_unswizzled (
  amd_dbgapi_segment_address_t segment_address, void *read, const void *write,
  size_t *size)
{
  if (!dispatch ().is_scratch_enabled ())
    return AMD_DBGAPI_STATUS_ERROR_MEMORY_ACCESS;

  amd_dbgapi_size_t limit = m_callbacks.scratch_memory_size ();
  amd_dbgapi_size_t offset = m_scratch_offset + segment_address;

  if ((offset + *size) > limit)
    {
      size_t max_size = offset < limit ? limit - offset : 0;
      if (max_size == 0 && *size != 0)
        return AMD_DBGAPI_STATUS_ERROR_MEMORY_ACCESS;
      *size = max_size;
    }

  amd_dbgapi_global_address_t global_address
    = m_callbacks.scratch_memory_base () + offset;

  if (read)
    return process ().read_global_memory_partial (global_address, read, size);
  else
    return process ().write_global_memory_partial (global_address, write,
                                                   size);
}

amd_dbgapi_status_t
wave_t::xfer_local_memory (amd_dbgapi_segment_address_t segment_address,
                           void *read, const void *write, size_t *size)
{
  /* The LDS is stored in the context save area.  */
  dbgapi_assert (queue ().is_suspended ());

  amd_dbgapi_size_t limit = m_cwsr_record->get_info (
    architecture_t::cwsr_record_t::query_kind_t::lds_size);
  amd_dbgapi_size_t offset = segment_address;

  if ((offset + *size) > limit)
    {
      size_t max_size = offset < limit ? limit - offset : 0;
      if (max_size == 0 && *size != 0)
        return AMD_DBGAPI_STATUS_ERROR_MEMORY_ACCESS;
      *size = max_size;
    }

  auto local_memory_base_address
    = group_leader ().m_cwsr_record->register_address (amdgpu_regnum_t::lds_0);

  if (!local_memory_base_address)
    error ("local memory is not accessible");

  amd_dbgapi_global_address_t global_address
    = *local_memory_base_address + offset;

  if (read)
    return process ().read_global_memory_partial (global_address, read, size);
  else
    return process ().write_global_memory_partial (global_address, write,
                                                   size);
}

amd_dbgapi_status_t
wave_t::xfer_segment_memory (const address_space_t &address_space,
                             amd_dbgapi_lane_id_t lane_id,
                             amd_dbgapi_segment_address_t segment_address,
                             void *read, const void *write, size_t *size)
{
  dbgapi_assert (!read != !write && "either read or write buffer");

  if (state () != AMD_DBGAPI_WAVE_STATE_STOP)
    return AMD_DBGAPI_STATUS_ERROR_WAVE_NOT_STOPPED;

  /* Zero-extend the segment address.  */
  segment_address &= utils::bit_mask (0, address_space.address_size () - 1);

  switch (address_space.kind ())
    {
    case address_space_t::private_swizzled:
      return xfer_private_memory_swizzled (segment_address, lane_id, read,
                                           write, size);

    case address_space_t::private_unswizzled:
      return xfer_private_memory_unswizzled (segment_address, read, write,
                                             size);

    case address_space_t::local:
      return xfer_local_memory (segment_address, read, write, size);

    case address_space_t::global:
      if (read)
        return process ().read_global_memory_partial (segment_address, read,
                                                      size);
      else
        return process ().write_global_memory_partial (segment_address, write,
                                                       size);

    default:
      error ("xfer_segment_memory from address space `%s' not supported",
             address_space.name ().c_str ());
    }
}

void
wave_t::raise_event (amd_dbgapi_event_kind_t event_kind)
{
  process_t &process = this->process ();
  event_t &event = process.create<event_t> (process, event_kind, id ());

  if (event_kind == AMD_DBGAPI_EVENT_KIND_WAVE_COMMAND_TERMINATED
      || event_kind == AMD_DBGAPI_EVENT_KIND_WAVE_STOP)
    m_last_stop_event_id = event.id ();

  process.enqueue_event (event);
}

const event_t *
wave_t::last_stop_event () const
{
  dbgapi_assert (state () == AMD_DBGAPI_WAVE_STATE_STOP);
  return process ().find (m_last_stop_event_id);
}

amd_dbgapi_wave_state_t
wave_t::client_visible_state () const
{
  amd_dbgapi_wave_state_t state = this->state ();

  if (state != AMD_DBGAPI_WAVE_STATE_STOP)
    return state;

  if (const event_t *event = last_stop_event ();
      !event || event->state () >= event_t::state_t::reported)
    return AMD_DBGAPI_WAVE_STATE_STOP;

  /* If the wave is stopped, but the wave stop event has not yet been
     reported to the client, return the last resumed state.  */
  return (stop_reason () & AMD_DBGAPI_WAVE_STOP_REASON_SINGLE_STEP)
           ? AMD_DBGAPI_WAVE_STATE_SINGLE_STEP
           : AMD_DBGAPI_WAVE_STATE_RUN;
}

amd_dbgapi_status_t
wave_t::get_info (amd_dbgapi_wave_info_t query, size_t value_size,
                  void *value) const
{
  switch (query)
    {
    case AMD_DBGAPI_WAVE_INFO_STATE:
      return utils::get_info (value_size, value, client_visible_state ());

    case AMD_DBGAPI_WAVE_INFO_STOP_REASON:
      return utils::get_info (value_size, value, stop_reason ());

    case AMD_DBGAPI_WAVE_INFO_DISPATCH:
      return utils::get_info (value_size, value, dispatch ().id ());

    case AMD_DBGAPI_WAVE_INFO_QUEUE:
      return utils::get_info (value_size, value, queue ().id ());

    case AMD_DBGAPI_WAVE_INFO_AGENT:
      return utils::get_info (value_size, value, agent ().id ());

    case AMD_DBGAPI_WAVE_INFO_PROCESS:
      return utils::get_info (value_size, value, process ().id ());

    case AMD_DBGAPI_WAVE_INFO_ARCHITECTURE:
      return utils::get_info (value_size, value, architecture ().id ());

    case AMD_DBGAPI_WAVE_INFO_PC:
      return utils::get_info (value_size, value, pc ());

    case AMD_DBGAPI_WAVE_INFO_EXEC_MASK:
      return utils::get_info (value_size, value, exec_mask ());

    case AMD_DBGAPI_WAVE_INFO_WORK_GROUP_COORD:
      return utils::get_info (value_size, value, m_group_ids);

    case AMD_DBGAPI_WAVE_INFO_WAVE_NUMBER_IN_WORK_GROUP:
      return utils::get_info (value_size, value, m_wave_in_group);

    case AMD_DBGAPI_WAVE_INFO_WATCHPOINTS:
      {
        amd_dbgapi_watchpoint_list_t list{};

        auto os_watch_ids = architecture ().triggered_watchpoints (*this);
        list.count = os_watch_ids.size ();

        list.watchpoint_ids = static_cast<amd_dbgapi_watchpoint_id_t *> (
          amd::dbgapi::allocate_memory (
            list.count * sizeof (amd_dbgapi_watchpoint_id_t)));

        if (list.count && !list.watchpoint_ids)
          return AMD_DBGAPI_STATUS_ERROR_CLIENT_CALLBACK;

        auto watchpoint_id = [this] (os_watch_id_t os_watch_id)
        {
          const watchpoint_t *watchpoint
            = process ().find_watchpoint (os_watch_id);
          if (!watchpoint)
            error ("kfd_watch_%d not set on %s", os_watch_id,
                   to_string (agent ().id ()).c_str ());
          return watchpoint->id ();
        };

        std::transform (os_watch_ids.begin (), os_watch_ids.end (),
                        list.watchpoint_ids, watchpoint_id);

        amd_dbgapi_status_t status = utils::get_info (value_size, value, list);
        if (status != AMD_DBGAPI_STATUS_SUCCESS)
          amd::dbgapi::deallocate_memory (list.watchpoint_ids);

        return status;
      }

    case AMD_DBGAPI_WAVE_INFO_LANE_COUNT:
      return utils::get_info (value_size, value, lane_count ());
    }
  return AMD_DBGAPI_STATUS_ERROR_INVALID_ARGUMENT;
}

} /* namespace amd::dbgapi */

using namespace amd::dbgapi;

amd_dbgapi_status_t AMD_DBGAPI
amd_dbgapi_wave_stop (amd_dbgapi_wave_id_t wave_id)
{
  TRACE_BEGIN (wave_id);
  TRY;

  if (!detail::is_initialized)
    return AMD_DBGAPI_STATUS_ERROR_NOT_INITIALIZED;

  wave_t *wave = find (wave_id);

  if (!wave)
    return AMD_DBGAPI_STATUS_ERROR_INVALID_WAVE_ID;

  if (wave->client_visible_state () == AMD_DBGAPI_WAVE_STATE_STOP)
    return AMD_DBGAPI_STATUS_ERROR_WAVE_STOPPED;

  if (wave->stop_requested ())
    return AMD_DBGAPI_STATUS_ERROR_WAVE_OUTSTANDING_STOP;

  scoped_queue_suspend_t suspend (wave->queue (), "stop wave");

  /* Look for the wave_id again, the wave may have exited.  */
  if (!(wave = find (wave_id)))
    return AMD_DBGAPI_STATUS_ERROR_INVALID_WAVE_ID;

  wave->set_state (AMD_DBGAPI_WAVE_STATE_STOP);

  return AMD_DBGAPI_STATUS_SUCCESS;

  CATCH;
  TRACE_END ();
}

amd_dbgapi_status_t AMD_DBGAPI
amd_dbgapi_wave_resume (amd_dbgapi_wave_id_t wave_id,
                        amd_dbgapi_resume_mode_t resume_mode)
{
  TRACE_BEGIN (wave_id, resume_mode);
  TRY;

  if (!detail::is_initialized)
    return AMD_DBGAPI_STATUS_ERROR_NOT_INITIALIZED;

  wave_t *wave = find (wave_id);

  if (!wave)
    return AMD_DBGAPI_STATUS_ERROR_INVALID_WAVE_ID;

  if (resume_mode != AMD_DBGAPI_RESUME_MODE_NORMAL
      && resume_mode != AMD_DBGAPI_RESUME_MODE_SINGLE_STEP)
    return AMD_DBGAPI_STATUS_ERROR_INVALID_ARGUMENT;

  if (wave->client_visible_state () != AMD_DBGAPI_WAVE_STATE_STOP)
    return AMD_DBGAPI_STATUS_ERROR_WAVE_NOT_STOPPED;

  if (wave->stop_reason () & ~wave_t::resumable_stop_reason_mask)
    return AMD_DBGAPI_STATUS_ERROR_WAVE_NOT_RESUMABLE;

  /* The wave is not resumable if the stop event is not yet processed.  */
  if (const event_t *event = wave->last_stop_event ();
      event && event->state () < event_t::state_t::processed)
    return AMD_DBGAPI_STATUS_ERROR_WAVE_NOT_RESUMABLE;

  if (wave->displaced_stepping ()
      && resume_mode != AMD_DBGAPI_RESUME_MODE_SINGLE_STEP)
    return AMD_DBGAPI_STATUS_ERROR_RESUME_DISPLACED_STEPPING;

  scoped_queue_suspend_t suspend (wave->queue (), "resume wave");

  /* Look for the wave_id again, the wave may have exited.  */
  if (!(wave = find (wave_id)))
    return AMD_DBGAPI_STATUS_ERROR_INVALID_WAVE_ID;

  wave->set_state (resume_mode == AMD_DBGAPI_RESUME_MODE_SINGLE_STEP
                     ? AMD_DBGAPI_WAVE_STATE_SINGLE_STEP
                     : AMD_DBGAPI_WAVE_STATE_RUN);

  return AMD_DBGAPI_STATUS_SUCCESS;

  CATCH;
  TRACE_END ();
}

amd_dbgapi_status_t AMD_DBGAPI
amd_dbgapi_wave_get_info (amd_dbgapi_wave_id_t wave_id,
                          amd_dbgapi_wave_info_t query, size_t value_size,
                          void *value)
{
  TRACE_BEGIN (wave_id, query, value_size, value);
  TRY;

  if (!detail::is_initialized)
    return AMD_DBGAPI_STATUS_ERROR_NOT_INITIALIZED;

  wave_t *wave = find (wave_id);

  if (!wave)
    return AMD_DBGAPI_STATUS_ERROR_INVALID_WAVE_ID;

  switch (query)
    {
    case AMD_DBGAPI_WAVE_INFO_STOP_REASON:
    case AMD_DBGAPI_WAVE_INFO_PC:
    case AMD_DBGAPI_WAVE_INFO_EXEC_MASK:
    case AMD_DBGAPI_WAVE_INFO_WATCHPOINTS:
      if (wave->client_visible_state () != AMD_DBGAPI_WAVE_STATE_STOP)
        return AMD_DBGAPI_STATUS_ERROR_WAVE_NOT_STOPPED;
    default:
      break;
    };

  return wave->get_info (query, value_size, value);

  CATCH;
  TRACE_END (make_query_ref (query, value));
}

amd_dbgapi_status_t AMD_DBGAPI
amd_dbgapi_process_wave_list (amd_dbgapi_process_id_t process_id,
                              size_t *wave_count, amd_dbgapi_wave_id_t **waves,
                              amd_dbgapi_changed_t *changed)
{
  TRACE_BEGIN (process_id, wave_count, waves, changed);
  TRY;

  if (!detail::is_initialized)
    return AMD_DBGAPI_STATUS_ERROR_NOT_INITIALIZED;

  std::vector<process_t *> processes;
  if (process_id != AMD_DBGAPI_PROCESS_NONE)
    {
      process_t *process = process_t::find (process_id);

      if (!process)
        return AMD_DBGAPI_STATUS_ERROR_INVALID_PROCESS_ID;

      if (amd_dbgapi_status_t status = process->update_queues ();
          status != AMD_DBGAPI_STATUS_SUCCESS)
        error ("process_t::update_queues failed (rc=%d)", status);

      processes.emplace_back (process);
    }
  else
    {
      for (auto &&process : process_t::all ())
        {
          if (amd_dbgapi_status_t status = process.update_queues ();
              status != AMD_DBGAPI_STATUS_SUCCESS)
            error ("process_t::update_queues failed (rc=%d)", status);

          processes.emplace_back (&process);
        }
    }

  std::vector<std::pair<process_t *, std::vector<queue_t *>>>
    queues_needing_resume;

  for (auto &&process : processes)
    {
      std::vector<queue_t *> queues;

      for (auto &&queue : process->range<queue_t> ())
        if (!queue.is_suspended ())
          queues.emplace_back (&queue);

      process->suspend_queues (queues, "refresh wave list");

      if (process->forward_progress_needed ())
        queues_needing_resume.emplace_back (process, std::move (queues));
    }

  amd_dbgapi_status_t status
    = utils::get_handle_list<wave_t> (processes, wave_count, waves, changed);

  for (auto &&[process, queues] : queues_needing_resume)
    process->resume_queues (queues, "refresh wave list");

  return status;

  CATCH;
  TRACE_END (make_ref (wave_count), make_ref (make_ref (waves), *wave_count),
             make_ref (changed));
}
