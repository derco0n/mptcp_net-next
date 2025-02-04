// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2019 Facebook
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <linux/bpf.h>
#include "bpf_helpers.h"

#define FUNCTION_NAME_LEN 64
#define FILE_NAME_LEN 128
#define TASK_COMM_LEN 16

typedef struct {
	int PyThreadState_frame;
	int PyThreadState_thread;
	int PyFrameObject_back;
	int PyFrameObject_code;
	int PyFrameObject_lineno;
	int PyCodeObject_filename;
	int PyCodeObject_name;
	int String_data;
	int String_size;
} OffsetConfig;

typedef struct {
	uintptr_t current_state_addr;
	uintptr_t tls_key_addr;
	OffsetConfig offsets;
	bool use_tls;
} PidData;

typedef struct {
	uint32_t success;
} Stats;

typedef struct {
	char name[FUNCTION_NAME_LEN];
	char file[FILE_NAME_LEN];
} Symbol;

typedef struct {
	uint32_t pid;
	uint32_t tid;
	char comm[TASK_COMM_LEN];
	int32_t kernel_stack_id;
	int32_t user_stack_id;
	bool thread_current;
	bool pthread_match;
	bool stack_complete;
	int16_t stack_len;
	int32_t stack[STACK_MAX_LEN];

	int has_meta;
	int metadata;
	char dummy_safeguard;
} Event;


struct bpf_elf_map {
	__u32 type;
	__u32 size_key;
	__u32 size_value;
	__u32 max_elem;
	__u32 flags;
};

typedef int pid_t;

typedef struct {
	void* f_back; // PyFrameObject.f_back, previous frame
	void* f_code; // PyFrameObject.f_code, pointer to PyCodeObject
	void* co_filename; // PyCodeObject.co_filename
	void* co_name; // PyCodeObject.co_name
} FrameData;

static inline __attribute__((__always_inline__)) void*
get_thread_state(void* tls_base, PidData* pidData)
{
	void* thread_state;
	int key;

	bpf_probe_read(&key, sizeof(key), (void*)(long)pidData->tls_key_addr);
	bpf_probe_read(&thread_state, sizeof(thread_state),
		       tls_base + 0x310 + key * 0x10 + 0x08);
	return thread_state;
}

static inline __attribute__((__always_inline__)) bool
get_frame_data(void* frame_ptr, PidData* pidData, FrameData* frame, Symbol* symbol)
{
	// read data from PyFrameObject
	bpf_probe_read(&frame->f_back,
		       sizeof(frame->f_back),
		       frame_ptr + pidData->offsets.PyFrameObject_back);
	bpf_probe_read(&frame->f_code,
		       sizeof(frame->f_code),
		       frame_ptr + pidData->offsets.PyFrameObject_code);

	// read data from PyCodeObject
	if (!frame->f_code)
		return false;
	bpf_probe_read(&frame->co_filename,
		       sizeof(frame->co_filename),
		       frame->f_code + pidData->offsets.PyCodeObject_filename);
	bpf_probe_read(&frame->co_name,
		       sizeof(frame->co_name),
		       frame->f_code + pidData->offsets.PyCodeObject_name);
	// read actual names into symbol
	if (frame->co_filename)
		bpf_probe_read_str(&symbol->file,
				   sizeof(symbol->file),
				   frame->co_filename + pidData->offsets.String_data);
	if (frame->co_name)
		bpf_probe_read_str(&symbol->name,
				   sizeof(symbol->name),
				   frame->co_name + pidData->offsets.String_data);
	return true;
}

struct bpf_elf_map SEC("maps") pidmap = {
	.type = BPF_MAP_TYPE_HASH,
	.size_key = sizeof(int),
	.size_value = sizeof(PidData),
	.max_elem = 1,
};

struct bpf_elf_map SEC("maps") eventmap = {
	.type = BPF_MAP_TYPE_HASH,
	.size_key = sizeof(int),
	.size_value = sizeof(Event),
	.max_elem = 1,
};

struct bpf_elf_map SEC("maps") symbolmap = {
	.type = BPF_MAP_TYPE_HASH,
	.size_key = sizeof(Symbol),
	.size_value = sizeof(int),
	.max_elem = 1,
};

struct bpf_elf_map SEC("maps") statsmap = {
	.type = BPF_MAP_TYPE_ARRAY,
	.size_key = sizeof(Stats),
	.size_value = sizeof(int),
	.max_elem = 1,
};

struct bpf_elf_map SEC("maps") perfmap = {
	.type = BPF_MAP_TYPE_PERF_EVENT_ARRAY,
	.size_key = sizeof(int),
	.size_value = sizeof(int),
	.max_elem = 32,
};

struct bpf_elf_map SEC("maps") stackmap = {
	.type = BPF_MAP_TYPE_STACK_TRACE,
	.size_key = sizeof(int),
	.size_value = sizeof(long long) * 127,
	.max_elem = 1000,
};

static inline __attribute__((__always_inline__)) int __on_event(struct pt_regs *ctx)
{
	uint64_t pid_tgid = bpf_get_current_pid_tgid();
	pid_t pid = (pid_t)(pid_tgid >> 32);
	PidData* pidData = bpf_map_lookup_elem(&pidmap, &pid);
	if (!pidData)
		return 0;

	int zero = 0;
	Event* event = bpf_map_lookup_elem(&eventmap, &zero);
	if (!event)
		return 0;

	event->pid = pid;

	event->tid = (pid_t)pid_tgid;
	bpf_get_current_comm(&event->comm, sizeof(event->comm));

	event->user_stack_id = bpf_get_stackid(ctx, &stackmap, BPF_F_USER_STACK);
	event->kernel_stack_id = bpf_get_stackid(ctx, &stackmap, 0);

	void* thread_state_current = (void*)0;
	bpf_probe_read(&thread_state_current,
		       sizeof(thread_state_current),
		       (void*)(long)pidData->current_state_addr);

	struct task_struct* task = (struct task_struct*)bpf_get_current_task();
	void* tls_base = (void*)task;

	void* thread_state = pidData->use_tls ? get_thread_state(tls_base, pidData)
		: thread_state_current;
	event->thread_current = thread_state == thread_state_current;

	if (pidData->use_tls) {
		uint64_t pthread_created;
		uint64_t pthread_self;
		bpf_probe_read(&pthread_self, sizeof(pthread_self), tls_base + 0x10);

		bpf_probe_read(&pthread_created,
			       sizeof(pthread_created),
			       thread_state + pidData->offsets.PyThreadState_thread);
		event->pthread_match = pthread_created == pthread_self;
	} else {
		event->pthread_match = 1;
	}

	if (event->pthread_match || !pidData->use_tls) {
		void* frame_ptr;
		FrameData frame;
		Symbol sym = {};
		int cur_cpu = bpf_get_smp_processor_id();

		bpf_probe_read(&frame_ptr,
			       sizeof(frame_ptr),
			       thread_state + pidData->offsets.PyThreadState_frame);

		int32_t* symbol_counter = bpf_map_lookup_elem(&symbolmap, &sym);
		if (symbol_counter == NULL)
			return 0;
#pragma unroll
		/* Unwind python stack */
		for (int i = 0; i < STACK_MAX_LEN; ++i) {
			if (frame_ptr && get_frame_data(frame_ptr, pidData, &frame, &sym)) {
				int32_t new_symbol_id = *symbol_counter * 64 + cur_cpu;
				int32_t *symbol_id = bpf_map_lookup_elem(&symbolmap, &sym);
				if (!symbol_id) {
					bpf_map_update_elem(&symbolmap, &sym, &zero, 0);
					symbol_id = bpf_map_lookup_elem(&symbolmap, &sym);
					if (!symbol_id)
						return 0;
				}
				if (*symbol_id == new_symbol_id)
					(*symbol_counter)++;
				event->stack[i] = *symbol_id;
				event->stack_len = i + 1;
				frame_ptr = frame.f_back;
			}
		}
		event->stack_complete = frame_ptr == NULL;
	} else {
		event->stack_complete = 1;
	}

	Stats* stats = bpf_map_lookup_elem(&statsmap, &zero);
	if (stats)
		stats->success++;

	event->has_meta = 0;
	bpf_perf_event_output(ctx, &perfmap, 0, event, offsetof(Event, metadata));
	return 0;
}

SEC("raw_tracepoint/kfree_skb")
int on_event(struct pt_regs* ctx)
{
	int i, ret = 0;
	ret |= __on_event(ctx);
	ret |= __on_event(ctx);
	ret |= __on_event(ctx);
	ret |= __on_event(ctx);
	ret |= __on_event(ctx);
	return ret;
}

char _license[] SEC("license") = "GPL";
