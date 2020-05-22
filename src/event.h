/* Copyright (c) 2019-2020 Advanced Micro Devices, Inc.

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

#ifndef _AMD_DBGAPI_EVENT_H
#define _AMD_DBGAPI_EVENT_H 1

#include "amd-dbgapi.h"
#include "handle_object.h"

#include <cstddef>
#include <string>

namespace amd
{
namespace dbgapi
{

class process_t;

class event_t : public detail::handle_object<amd_dbgapi_event_id_t>
{
public:
  /* Breakpoint resume event.  */
  event_t (amd_dbgapi_event_id_t event_id, process_t &process,
           amd_dbgapi_event_kind_t event_kind,
           amd_dbgapi_breakpoint_id_t breakpoint_id,
           amd_dbgapi_client_thread_id_t client_thread_id);

  /* Code object list updated event.  */
  event_t (amd_dbgapi_event_id_t event_id, process_t &process,
           amd_dbgapi_event_kind_t event_kind,
           amd_dbgapi_event_id_t breakpoint_resume_event_id);

  /* Runtime event. */
  event_t (amd_dbgapi_event_id_t event_id, process_t &process,
           amd_dbgapi_event_kind_t event_kind,
           amd_dbgapi_runtime_state_t runtime_state);

  /* Wave stop / command terminated event.  */
  event_t (amd_dbgapi_event_id_t event_id, process_t &process,
           amd_dbgapi_event_kind_t event_kind, amd_dbgapi_wave_id_t wave_id);

  amd_dbgapi_event_kind_t kind () const { return m_event_kind; }

  void processed ();

  amd_dbgapi_status_t get_info (amd_dbgapi_event_info_t query,
                                size_t value_size, void *value) const;

  std::string pretty_printer_string () const;

  process_t &process () const { return m_process; }

private:
  amd_dbgapi_event_kind_t const m_event_kind;

  union
  {
    /* Breakpoint resume event.  */
    struct
    {
      amd_dbgapi_breakpoint_id_t breakpoint_id;
      amd_dbgapi_client_thread_id_t client_thread_id;
    } breakpoint_resume_event;

    /* Code object list updated event.  */
    struct
    {
      amd_dbgapi_event_id_t breakpoint_resume_event_id;
    } code_object_list_updated_event;

    /* Runtime event.  */
    struct
    {
      amd_dbgapi_runtime_state_t runtime_state;
    } runtime_event;

    /* Wave stop / command terminated event .  */
    struct
    {
      amd_dbgapi_wave_id_t wave_id;
    } wave_event;

  } const m_data;

  process_t &m_process;
};

} /* namespace dbgapi */
} /* namespace amd */

#endif /* _AMD_DBGAPI_EVENT_H */
