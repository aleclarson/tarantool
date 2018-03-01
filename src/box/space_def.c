/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "space_def.h"
#include "diag.h"
#include "error.h"

const struct space_opts space_opts_default = {
	/* .temporary = */ false,
};

const struct opt_def space_opts_reg[] = {
	OPT_DEF("temporary", OPT_BOOL, struct space_opts, temporary),
	OPT_END,
};

/**
 * Size of the space_def.
 * @param name_len Length of the space name.
 * @param field_names_size Size of all names.
 * @param field_count Space field count.
 * @param[out] names_offset Offset from the beginning of a def to
 *             a field names memory.
 * @param[out] fields_offset Offset from the beginning of a def to
 *             a fields array.
 * @retval Size in bytes.
 */
static inline size_t
space_def_sizeof(uint32_t name_len, uint32_t field_names_size,
		 uint32_t field_count, uint32_t *names_offset,
		 uint32_t *fields_offset)
{
	*fields_offset = sizeof(struct space_def) + name_len + 1;
	*names_offset = *fields_offset + field_count * sizeof(struct field_def);
	return *names_offset + field_names_size;
}

struct space_def *
space_def_dup(const struct space_def *src)
{
	uint32_t names_offset, fields_offset;
	uint32_t field_names_size = 0;
	for (uint32_t i = 0; i < src->field_count; ++i)
		field_names_size += strlen(src->fields[i].name) + 1;
	size_t size = space_def_sizeof(strlen(src->name), field_names_size,
				       src->field_count, &names_offset,
				       &fields_offset);
	struct space_def *ret = (struct space_def *) malloc(size);
	if (ret == NULL) {
		diag_set(OutOfMemory, size, "malloc", "ret");
		return NULL;
	}
	memcpy(ret, src, size);
	char *name_pos = (char *)ret + names_offset;
	if (src->field_count > 0) {
		ret->fields = (struct field_def *)((char *)ret + fields_offset);
		for (uint32_t i = 0; i < src->field_count; ++i) {
			ret->fields[i].name = name_pos;
			name_pos += strlen(name_pos) + 1;
		}
	}
	tuple_dictionary_ref(ret->dict);
	return ret;
}

struct space_def *
space_def_new(uint32_t id, uint32_t uid, uint32_t exact_field_count,
	      const char *name, uint32_t name_len,
	      const char *engine_name, uint32_t engine_len,
	      const struct space_opts *opts, const struct field_def *fields,
	      uint32_t field_count)
{
	uint32_t field_names_size = 0;
	for (uint32_t i = 0; i < field_count; ++i)
		field_names_size += strlen(fields[i].name) + 1;
	uint32_t names_offset, fields_offset;
	size_t size = space_def_sizeof(name_len, field_names_size, field_count,
				       &names_offset, &fields_offset);
	struct space_def *def = (struct space_def *) malloc(size);
	if (def == NULL) {
		diag_set(OutOfMemory, size, "malloc", "def");
		return NULL;
	}
	assert(name_len <= BOX_NAME_MAX);
	assert(engine_len <= ENGINE_NAME_MAX);
	def->dict = tuple_dictionary_new(fields, field_count);
	if (def->dict == NULL) {
		free(def);
		return NULL;
	}
	def->id = id;
	def->uid = uid;
	def->exact_field_count = exact_field_count;
	memcpy(def->name, name, name_len);
	def->name[name_len] = 0;
	memcpy(def->engine_name, engine_name, engine_len);
	def->engine_name[engine_len] = 0;
	def->opts = *opts;
	def->field_count = field_count;
	if (field_count == 0) {
		def->fields = NULL;
	} else {
		char *name_pos = (char *)def + names_offset;
		def->fields = (struct field_def *)((char *)def + fields_offset);
		for (uint32_t i = 0; i < field_count; ++i) {
			def->fields[i].name = name_pos;
			uint32_t len = strlen(fields[i].name);
			memcpy(def->fields[i].name, fields[i].name, len);
			def->fields[i].name[len] = 0;
			def->fields[i].type = fields[i].type;
			name_pos += len + 1;
			def->fields[i].is_nullable = fields[i].is_nullable;
		}
	}
	return def;
}

int
space_def_check_compatibility(const struct space_def *old_def,
			      const struct space_def *new_def,
			      bool is_space_empty)
{
	if (strcmp(new_def->engine_name, old_def->engine_name) != 0) {
		diag_set(ClientError, ER_ALTER_SPACE, old_def->name,
			 "can not change space engine");
		return -1;
	}
	if (new_def->id != old_def->id) {
		diag_set(ClientError, ER_ALTER_SPACE, old_def->name,
			 "space id is immutable");
		return -1;
	}
	if (is_space_empty)
		return 0;

	if (new_def->exact_field_count != 0 &&
	    new_def->exact_field_count != old_def->exact_field_count) {
		diag_set(ClientError, ER_ALTER_SPACE, old_def->name,
			 "can not change field count on a non-empty space");
		return -1;
	}
	if (new_def->opts.temporary != old_def->opts.temporary) {
		diag_set(ClientError, ER_ALTER_SPACE, old_def->name,
			 "can not switch temporary flag on a non-empty space");
		return -1;
	}
	uint32_t field_count = MIN(new_def->field_count, old_def->field_count);
	for (uint32_t i = 0; i < field_count; ++i) {
		enum field_type old_type = old_def->fields[i].type;
		enum field_type new_type = new_def->fields[i].type;
		if (!field_type1_contains_type2(new_type, old_type) &&
		    !field_type1_contains_type2(old_type, new_type)) {
			const char *msg =
				tt_sprintf("Can not change a field type from "\
					   "%s to %s on a not empty space",
					   field_type_strs[old_type],
					   field_type_strs[new_type]);
			diag_set(ClientError, ER_ALTER_SPACE, old_def->name,
				 msg);
			return -1;
		}
	}
	return 0;
}

