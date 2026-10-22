# SPDX-FileCopyrightText: 2026 gdxsv contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# gdxsv sources and their build settings, included from the top-level CMakeLists.txt.
set(GDXSV_TEXTURE_PACK_PASS "" CACHE STRING "Gdxsv texture pack password")
option(ENABLE_SLEEP_BENCHMARK "Enable sleep timing benchmark logging" OFF)
option(GDXSV_FP_COMPAT "gdxsv: compute SH4 fmac as fmul + fadd (no fused multiply-add) on every CPU and backend so replays and netplay stay in sync" ON)
option(MSVC_BREAKPAD "Windows MSVC: Build breakpad client" OFF)

if(ENABLE_SLEEP_BENCHMARK)
	target_compile_definitions(${PROJECT_NAME} PRIVATE SLEEP_BENCHMARK)
endif()
if(NOT "${GDXSV_TEXTURE_PACK_PASS}" STREQUAL "")
	target_compile_definitions(${PROJECT_NAME} PRIVATE GDXSV_TEXTURE_PACK_PASS="${GDXSV_TEXTURE_PACK_PASS}")
endif()
if(GDXSV_FP_COMPAT)
	# gdxsv: a*b+c must round twice on every host. Keep the compiler from fusing it
	# (fmac in the interpreter and canonical, innerProduct for fipr/ftrv ...).
	target_compile_definitions(${PROJECT_NAME} PRIVATE GDXSV_FP_COMPAT)
	if(NOT MSVC)
		target_compile_options(${PROJECT_NAME} PRIVATE -ffp-contract=off)
	endif()
endif()

if(MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/Zi>")
	target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG>")
	target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/OPT:REF>")
	target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/OPT:ICF>")
endif()

if(USE_BREAKPAD AND NOT LIBRETRO AND MSVC AND MSVC_BREAKPAD)
	target_sources(${PROJECT_NAME} PRIVATE
		core/deps/breakpad/src/client/windows/common/ipc_protocol.h
		core/deps/breakpad/src/client/windows/crash_generation/crash_generation_client.h
		core/deps/breakpad/src/client/windows/crash_generation/crash_generation_client.cc
		core/deps/breakpad/src/client/windows/crash_generation/client_info.h
		core/deps/breakpad/src/client/windows/crash_generation/client_info.cc
		core/deps/breakpad/src/client/windows/common/auto_critical_section.h
		core/deps/breakpad/src/client/windows/handler/exception_handler.h
		core/deps/breakpad/src/client/windows/handler/exception_handler.cc
		core/deps/breakpad/src/common/scoped_ptr.h
		core/deps/breakpad/src/common/windows/string_utils-inl.h
		core/deps/breakpad/src/common/windows/string_utils.cc
		core/deps/breakpad/src/common/windows/guid_string.h
		core/deps/breakpad/src/common/windows/guid_string.cc
		core/deps/breakpad/src/google_breakpad/common/minidump_format.h
		core/deps/breakpad/src/google_breakpad/common/breakpad_types.h
	)
	set_source_files_properties(
		core/deps/breakpad/src/client/windows/handler/exception_handler.cc
		core/deps/breakpad/src/client/windows/crash_generation/crash_generation_client.cc
		PROPERTIES COMPILE_OPTIONS -DUNICODE -D_UNICODE -g
	)
	target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/deps/breakpad/src)
	target_compile_definitions(${PROJECT_NAME} PRIVATE USE_BREAKPAD)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_PTHREAD)
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/protobuf-3.13.0/src)
target_sources(${PROJECT_NAME} PRIVATE
		core/deps/protobuf-3.13.0/src/google/protobuf/any.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/any.h
		core/deps/protobuf-3.13.0/src/google/protobuf/any.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/any.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/any_lite.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/api.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/api.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/arena.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/arena.h
		core/deps/protobuf-3.13.0/src/google/protobuf/arenastring.h
		core/deps/protobuf-3.13.0/src/google/protobuf/arena_impl.h
		core/deps/protobuf-3.13.0/src/google/protobuf/descriptor.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/descriptor.h
		core/deps/protobuf-3.13.0/src/google/protobuf/descriptor.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/descriptor.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/descriptor_database.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/descriptor_database.h
		core/deps/protobuf-3.13.0/src/google/protobuf/duration.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/duration.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/dynamic_message.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/dynamic_message.h
		core/deps/protobuf-3.13.0/src/google/protobuf/empty.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/empty.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/extension_set.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/extension_set.h
		core/deps/protobuf-3.13.0/src/google/protobuf/extension_set_heavy.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/extension_set_inl.h
		core/deps/protobuf-3.13.0/src/google/protobuf/field_mask.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/field_mask.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_enum_reflection.h
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_enum_util.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_enum_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_message_reflection.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_message_reflection.h
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_message_table_driven.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_message_table_driven.h
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_message_table_driven_lite.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_message_table_driven_lite.h
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_message_util.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/generated_message_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/has_bits.h
		core/deps/protobuf-3.13.0/src/google/protobuf/implicit_weak_message.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/implicit_weak_message.h
		core/deps/protobuf-3.13.0/src/google/protobuf/inlined_string_field.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/coded_stream.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/io/coded_stream.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/gzip_stream.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/io/gzip_stream.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/io_win32.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/io/io_win32.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/package_info.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/printer.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/io/printer.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/strtod.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/io/strtod.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/tokenizer.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/io/tokenizer.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/zero_copy_stream.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/io/zero_copy_stream.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/zero_copy_stream_impl.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/io/zero_copy_stream_impl.h
		core/deps/protobuf-3.13.0/src/google/protobuf/io/zero_copy_stream_impl_lite.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/io/zero_copy_stream_impl_lite.h
		core/deps/protobuf-3.13.0/src/google/protobuf/map.h
		core/deps/protobuf-3.13.0/src/google/protobuf/map_entry.h
		core/deps/protobuf-3.13.0/src/google/protobuf/map_entry_lite.h
		core/deps/protobuf-3.13.0/src/google/protobuf/map_field.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/map_field.h
		core/deps/protobuf-3.13.0/src/google/protobuf/map_field_inl.h
		core/deps/protobuf-3.13.0/src/google/protobuf/map_field_lite.h
		core/deps/protobuf-3.13.0/src/google/protobuf/map_type_handler.h
		core/deps/protobuf-3.13.0/src/google/protobuf/message.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/message.h
		core/deps/protobuf-3.13.0/src/google/protobuf/message_lite.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/message_lite.h
		core/deps/protobuf-3.13.0/src/google/protobuf/metadata.h
		core/deps/protobuf-3.13.0/src/google/protobuf/metadata_lite.h
		core/deps/protobuf-3.13.0/src/google/protobuf/package_info.h
		core/deps/protobuf-3.13.0/src/google/protobuf/parse_context.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/parse_context.h
		core/deps/protobuf-3.13.0/src/google/protobuf/port.h
		core/deps/protobuf-3.13.0/src/google/protobuf/reflection.h
		core/deps/protobuf-3.13.0/src/google/protobuf/reflection_internal.h
		core/deps/protobuf-3.13.0/src/google/protobuf/reflection_ops.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/reflection_ops.h
		core/deps/protobuf-3.13.0/src/google/protobuf/repeated_field.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/repeated_field.h
		core/deps/protobuf-3.13.0/src/google/protobuf/service.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/service.h
		core/deps/protobuf-3.13.0/src/google/protobuf/source_context.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/source_context.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/struct.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/struct.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/bytestream.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/bytestream.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/callback.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/casts.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/common.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/common.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/fastmem.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/hash.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/int128.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/int128.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/logging.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/macros.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/map_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/mathutil.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/mutex.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/once.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/platform_macros.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/port.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/status.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/status.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/statusor.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/statusor.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/status_macros.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/stl_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/stringpiece.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/stringpiece.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/stringprintf.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/stringprintf.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/structurally_valid.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/strutil.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/strutil.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/substitute.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/substitute.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/template_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/time.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/stubs/time.h
		core/deps/protobuf-3.13.0/src/google/protobuf/text_format.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/text_format.h
		core/deps/protobuf-3.13.0/src/google/protobuf/timestamp.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/timestamp.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/type.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/type.pb.h
		core/deps/protobuf-3.13.0/src/google/protobuf/unknown_field_set.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/unknown_field_set.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/delimited_message_util.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/delimited_message_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/field_comparator.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/field_comparator.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/field_mask_util.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/field_mask_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/constants.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/datapiece.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/datapiece.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/default_value_objectwriter.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/default_value_objectwriter.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/error_listener.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/error_listener.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/expecting_objectwriter.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/field_mask_utility.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/field_mask_utility.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/json_escaping.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/json_escaping.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/json_objectwriter.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/json_objectwriter.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/json_stream_parser.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/json_stream_parser.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/location_tracker.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/mock_error_listener.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/object_location_tracker.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/object_source.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/object_writer.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/object_writer.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/protostream_objectsource.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/protostream_objectsource.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/protostream_objectwriter.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/protostream_objectwriter.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/proto_writer.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/proto_writer.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/structured_objectwriter.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/type_info.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/type_info.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/utility.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/internal/utility.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/json_util.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/json_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/message_differencer.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/message_differencer.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/package_info.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/time_util.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/time_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/type_resolver.h
		core/deps/protobuf-3.13.0/src/google/protobuf/util/type_resolver_util.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/util/type_resolver_util.h
		core/deps/protobuf-3.13.0/src/google/protobuf/wire_format.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/wire_format.h
		core/deps/protobuf-3.13.0/src/google/protobuf/wire_format_lite.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/wire_format_lite.h
		core/deps/protobuf-3.13.0/src/google/protobuf/wrappers.pb.cc
		core/deps/protobuf-3.13.0/src/google/protobuf/wrappers.pb.h)

target_sources(${PROJECT_NAME} PRIVATE
		core/gdxsv/gdx_rpc.h
		core/gdxsv/gdxsv.cpp
		core/gdxsv/gdxsv.h
		core/gdxsv/gdxsv_config.cpp
		core/gdxsv/gdxsv_config.h
		core/gdxsv/gdxsv_sh4_block_map.cpp
		core/gdxsv/gdxsv_sh4_hooks.cpp
		core/gdxsv/gdxsv_sh4_hooks.h
		core/gdxsv/gdxsv.pb.cc
		core/gdxsv/gdxsv.pb.h
		core/gdxsv/gdxsv_audio.cpp
		core/gdxsv/gdxsv_audio_hooks.h
		core/gdxsv/gdxsv_custom_texture_cells.cpp
		core/gdxsv/gdxsv_custom_texture_cells.h
		core/gdxsv/gdxsv_custom_texture_hooks.h
		core/gdxsv/gdxsv_custom_texture_source.cpp
		core/gdxsv/gdxsv_custom_texture_source.h
		core/gdxsv/gdxsv_custom_texture_update.cpp
		core/gdxsv/gdxsv_custom_texture_update.h
		core/gdxsv/gdxsv_backend_replay.cpp
		core/gdxsv/gdxsv_backend_replay.h
		core/gdxsv/gdxsv_backend_rollback.cpp
		core/gdxsv/gdxsv_backend_rollback.h
		core/gdxsv/gdxsv_ggpo.cpp
		core/gdxsv/gdxsv_ggpo.h
		core/gdxsv/gdxsv_ggpo_hooks.cpp
		core/gdxsv/gdxsv_ggpo_hooks.h
		core/gdxsv/gdxsv_backend_tcp.cpp
		core/gdxsv/gdxsv_backend_tcp.h
		core/gdxsv/gdxsv_backend_udp.cpp
		core/gdxsv/gdxsv_backend_udp.h
		core/gdxsv/gdxsv_emu_hooks.cpp
		core/gdxsv/gdxsv_emu_hooks.h
		core/gdxsv/gdxsv_frame_limiter.cpp
		core/gdxsv/gdxsv_frame_limiter.h
		core/gdxsv/gdxsv_gui_settings.cpp
		core/gdxsv/gdxsv_gui_settings.h
		core/gdxsv/gdxsv_https_latency.h
		core/gdxsv/gdxsv_input.cpp
		core/gdxsv/gdxsv_input_hooks.h
		core/gdxsv/gdxsv_key_display.cpp
		core/gdxsv/gdxsv_key_display.h
		core/gdxsv/gdxsv_projectile_view.cpp
		core/gdxsv/gdxsv_projectile_view.h
		core/gdxsv/gdxsv_slowdown.cpp
		core/gdxsv/gdxsv_slowdown.h
		core/gdxsv/gdxsv_network.cpp
		core/gdxsv/gdxsv_network.h
		core/gdxsv/gdxsv_null_imgui_driver.h
		core/gdxsv/juice_helper.c
		core/gdxsv/juice_helper.h
		core/gdxsv/gdxsv_patch.inc
		core/gdxsv/gdxsv_platform.h
		core/gdxsv/gdxsv_prof.cpp
		core/gdxsv/gdxsv_prof.h
		core/gdxsv/gdxsv_replay_util.cpp
		core/gdxsv/gdxsv_replay_util.h
		core/gdxsv/gdxsv_save_state.cpp
		core/gdxsv/gdxsv_save_state.h
		core/gdxsv/gdxsv_sleep.cpp
		core/gdxsv/gdxsv_sleep.h
		core/gdxsv/gdxsv_multi_pov.cpp
		core/gdxsv/gdxsv_multi_pov_audio.cpp
		core/gdxsv/gdxsv_multi_pov_audio.h
		core/gdxsv/gdxsv_multi_pov.h
		core/gdxsv/gdxsv_multi_pov_launch.cpp
		core/gdxsv/gdxsv_multi_pov_viewer.cpp
		core/gdxsv/gdxsv_multi_pov_viewer.h
		core/gdxsv/gdxsv_multi_pov_viewer_vulkan.h
		core/gdxsv/gdxsv_video_hooks.h
		core/gdxsv/gdxsv_spectate_sync.cpp
		core/gdxsv/gdxsv_spectate_sync.h
		core/gdxsv/gdxsv_spectator_downlink.cpp
		core/gdxsv/gdxsv_spectator_downlink.h
		core/gdxsv/gdxsv_spectator_uplink.cpp
		core/gdxsv/gdxsv_spectator_uplink.h
		core/gdxsv/gdxsv_translation.cpp
		core/gdxsv/gdxsv_translation.h
		core/gdxsv/gdxsv_translation_patch.inc
		core/gdxsv/gdxsv_update.cpp
		core/gdxsv/gdxsv_update.h
		core/gdxsv/lbs_message.h
		core/gdxsv/libs.h
		core/gdxsv/mcs_message.h)

set_source_files_properties(core/gdxsv/juice_helper.c PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/core/deps/libjuice/include/juice")

if(WIN32)
	set(CMAKE_VS_SDK_INCLUDE_DIRECTORIES ${CUSTOM_INCLUDE_DIRECTORIES})
	target_sources(${PROJECT_NAME} PRIVATE core/gdxsv/textures.rc)
	if(NOT WINDOWS_STORE)
		target_sources(${PROJECT_NAME} PRIVATE core/gdxsv/gdxsv_https_latency_win.cpp)
	endif()
endif()
if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT NINTENDO_SWITCH)
	target_sources(${PROJECT_NAME} PRIVATE core/gdxsv/gdxsv_https_latency_curl.cpp)
endif()
if(APPLE)
	set(ASSETS core/gdxsv/Textures)
	target_sources(${PROJECT_NAME} PRIVATE
			core/gdxsv/gdxsv_https_latency_apple.mm
			${ASSETS})
	source_group("Resources" FILES ${ASSETS})
	set_source_files_properties(${ASSETS} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
	if(NOT IOS)
		target_sources(${PROJECT_NAME} PRIVATE core/gdxsv/gdxsv_update_apple.mm)
	endif()
endif()

# Four-screen video receivers
if(WIN32 AND NOT WINDOWS_STORE)
	target_sources(${PROJECT_NAME} PRIVATE core/gdxsv/gdxsv_multi_pov_viewer_win.cpp core/gdxsv/gdxsv_multi_pov_viewer_vulkan_win.cpp)
elseif(APPLE AND NOT IOS)
	target_sources(${PROJECT_NAME} PRIVATE core/gdxsv/gdxsv_multi_pov_viewer_apple.mm core/gdxsv/gdxsv_multi_pov_viewer_vulkan.cpp)
endif()

# Host services: machine id, connection medium, open URL
if(WIN32)
	target_sources(${PROJECT_NAME} PRIVATE core/gdxsv/gdxsv_platform_win.cpp)
	if(NOT WINDOWS_STORE)
		target_link_libraries(${PROJECT_NAME} PRIVATE iphlpapi.lib)
	endif()
elseif(APPLE AND NOT IOS)
	target_sources(${PROJECT_NAME} PRIVATE core/gdxsv/gdxsv_platform_apple.mm)
	find_library(SYSTEMCONFIGURATION_LIBRARY SystemConfiguration)
	target_link_libraries(${PROJECT_NAME} PRIVATE ${SYSTEMCONFIGURATION_LIBRARY})
elseif(UNIX AND NOT ANDROID)
	target_sources(${PROJECT_NAME} PRIVATE core/gdxsv/gdxsv_platform_linux.cpp)
endif()
