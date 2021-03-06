#include "serialization.h"
#include "array.h"
#include "dictionary.h"
#include "utils.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    unsigned char* start;
    unsigned char* end;
    unsigned char* ptr;
    size_t num_ports;
    spc_port_t* ports;
} writer_t;

typedef struct {
    unsigned char* end;
    unsigned char* ptr;
    size_t next_port;
    size_t num_ports;
    spc_port_t* ports;
} reader_t;

void spc_ensure_space(writer_t* writer, size_t num_bytes)
{
    if (writer->ptr + num_bytes > writer->end) {
        size_t new_size = (writer->end - writer->start) + num_bytes;
        void* buf = realloc(writer->start, new_size);
        writer->ptr = buf + (writer->ptr - writer->start);
        writer->start = buf;
        writer->end = buf + new_size;
    }
}

size_t spc_write(writer_t* writer, void* buf, size_t len)
{
    spc_ensure_space(writer, len);

    memcpy(writer->ptr, buf, len);
    writer->ptr += len;

    return len;
}

size_t spc_write_padded(writer_t* writer, const void* buf, size_t len)
{
    size_t remainder = (4 - (len % 4)) % 4;

    spc_ensure_space(writer, len + remainder);

    memcpy(writer->ptr, buf, len);
    writer->ptr += len;
    memset(writer->ptr, 0, remainder);
    writer->ptr += remainder;

    return len + remainder;
}

size_t spc_write_str(writer_t* writer, const char* str)
{
    return spc_write_padded(writer, str, strlen(str) + 1);
}

size_t spc_write_uint32(writer_t* writer, uint32_t val)
{
    // TODO replace with
    //    spc_write(writer, &val, 4);
    spc_ensure_space(writer, 4);

    memcpy(writer->ptr, &val, 4);
    writer->ptr += 4;

    return 4;
}

size_t spc_write_uint64(writer_t* writer, uint64_t val)
{
    spc_ensure_space(writer, 8);

    memcpy(writer->ptr, &val, 8);
    writer->ptr += 8;

    return 8;
}

size_t spc_write_int64(writer_t* writer, uint64_t val)
{
    spc_ensure_space(writer, 8);

    memcpy(writer->ptr, &val, 8);
    writer->ptr += 8;

    return 8;
}

size_t spc_write_double(writer_t* writer, double val)
{
    spc_ensure_space(writer, 8);

    memcpy(writer->ptr, &val, 8);
    writer->ptr += 8;

    return 8;
}

void spc_write_port(writer_t* writer, spc_port_t port)
{
    writer->ports = realloc(writer->ports, (writer->num_ports + 1) * sizeof(spc_port_t));
    writer->ports[writer->num_ports] = port;
    writer->num_ports += 1;
}

size_t spc_write_array(writer_t* writer, spc_array_t* array);
size_t spc_write_dict(writer_t* writer, spc_dictionary_t* dict);

size_t spc_serialize_value(writer_t* writer, spc_value_t value)
{
    size_t bytes_written = 0;
    bytes_written += spc_write_uint32(writer, value.type);
    switch (value.type) {
        case SPC_TYPE_NULL:
            break;
        case SPC_TYPE_BOOL:
            bytes_written += spc_write_uint32(writer, value.value.u64);
            break;
        case SPC_TYPE_UINT64:
            bytes_written += spc_write_uint64(writer, value.value.u64);
            break;
        case SPC_TYPE_INT64:
            bytes_written += spc_write_int64(writer, value.value.i64);
            break;
        case SPC_TYPE_DOUBLE:
            bytes_written += spc_write_double(writer, value.value.dbl);
            break;
        case SPC_TYPE_STRING:
            bytes_written += spc_write_uint32(writer, strlen(value.value.str) + 1);
            bytes_written += spc_write_str(writer, value.value.str);
            break;
        case SPC_TYPE_ARRAY:
            bytes_written += spc_write_array(writer, value.value.array);
            break;
        case SPC_TYPE_DICT:
            bytes_written += spc_write_dict(writer, value.value.dict);
            break;
        case SPC_TYPE_FD:
        case SPC_TYPE_SEND_PORT:
        case SPC_TYPE_RECV_PORT:
            spc_write_port(writer, value.value.port);
            break;
        case SPC_TYPE_UUID:
            bytes_written += spc_write(writer, value.value.ptr, 0x10);
            break;
        case SPC_TYPE_DATA:
            bytes_written += spc_write_uint32(writer, value.value.data.size);
            bytes_written += spc_write_padded(writer, value.value.data.ptr, value.value.data.size);
            break;
        default:
            printf("Unsupported value type: 0x%x\n", value.type);
    }

    return bytes_written;
}

size_t spc_write_array(writer_t* writer, spc_array_t* array)
{
    size_t bytes_written = 0;
    ptrdiff_t bytesize_offset = writer->ptr - writer->start;
    spc_write_uint32(writer, 0);            // placeholder for byte size

    bytes_written += spc_write_uint32(writer, array->length);

    for (size_t i = 0; i < array->length; i++) {
        bytes_written += spc_serialize_value(writer, array->values[i]);
    }

    // Fill in correct byte size
    *(uint32_t*)(writer->start + bytesize_offset) = bytes_written;
    return bytes_written + 4;
}

size_t spc_write_dict(writer_t* writer, spc_dictionary_t* dict)
{
    size_t bytes_written = 0;
    ptrdiff_t bytesize_offset = writer->ptr - writer->start;
    spc_write_uint32(writer, 0);            // placeholder for byte size

    bytes_written += spc_write_uint32(writer, dict->num_items);

    for (spc_dictionary_item_t* item = dict->items; item != NULL; item = item->next) {
        bytes_written += spc_write_str(writer, item->key);
        bytes_written += spc_serialize_value(writer, item->value);
    }

    // Fill in correct byte size
    *(uint32_t*)(writer->start + bytesize_offset) = bytes_written;
    return bytes_written + 4;
}

spc_mach_message_t* spc_serialize(spc_message_t* msg)
{
    spc_mach_message_t* mach_msg;

    size_t actual_size, content_size, initial_size = msg->content->num_items * 32;      // heuristic

    writer_t writer;
    writer.start = malloc(initial_size);
    writer.end = writer.start + initial_size;
    writer.ptr = writer.start;
    writer.ports = NULL;
    writer.num_ports = 0;

    spc_write(&writer, "CPX@\x05\x00\x00\x00", 8);
    spc_write_uint32(&writer, SPC_TYPE_DICT);
    spc_write_dict(&writer, msg->content);

    content_size = writer.ptr - writer.start;
    char* ptr;

    if (writer.num_ports != 0) {
        // Must create a complex messge
        actual_size = sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) + writer.num_ports * sizeof(mach_msg_port_descriptor_t) + content_size;
        mach_msg = malloc(actual_size);
        mach_msg->header.msgh_bits = MACH_MSGH_BITS_COMPLEX;

        ptr = (char*)mach_msg + sizeof(mach_msg_header_t);
        mach_msg_body_t* body = (mach_msg_body_t*)ptr;
        body->msgh_descriptor_count = writer.num_ports;
        ptr += sizeof(mach_msg_body_t);

        for (size_t i = 0; i < writer.num_ports; i++) {
            mach_msg_port_descriptor_t* descriptor = (mach_msg_port_descriptor_t*)ptr;
            descriptor->type = MACH_MSG_PORT_DESCRIPTOR;
            descriptor->name = writer.ports[i].name;
            descriptor->disposition = writer.ports[i].type;
            ptr += sizeof(mach_msg_port_descriptor_t);
        }
    } else {
        actual_size = sizeof(mach_msg_header_t) + content_size;
        mach_msg = malloc(actual_size);
        mach_msg->header.msgh_bits = 0;
        ptr = (char*)mach_msg->buf;
    }

    // Fill in the mach message
    mach_msg->header.msgh_remote_port = msg->remote_port.name;
    mach_msg->header.msgh_local_port  = msg->local_port.name;
    mach_msg->header.msgh_id          = msg->id;
    mach_msg->header.msgh_size        = actual_size;
    mach_msg->header.msgh_bits       |= MACH_MSGH_BITS(msg->remote_port.type, msg->local_port.type);
    memcpy(ptr, writer.start, content_size);

    free(writer.start);

    return mach_msg;
}

void* spc_read(reader_t* reader, size_t len)
{
    // Let it crash if the received message is invalid...
    if (reader->ptr + len > reader->end) {
        printf("OOB read in spc_read\n");
        abort();
    }

    void* res = reader->ptr;
    reader->ptr += len;
    return res;
}

uint64_t spc_read_uint64(reader_t* reader)
{
    return *(uint64_t*)spc_read(reader, 8);
}

int64_t spc_read_int64(reader_t* reader)
{
    return *(int64_t*)spc_read(reader, 8);
}

double spc_read_double(reader_t* reader)
{
    return *(double*)spc_read(reader, 8);
}

uint32_t spc_read_uint32(reader_t* reader)
{
    return *(uint32_t*)spc_read(reader, 4);
}

void* spc_read_padded(reader_t* reader, size_t size)
{
    size_t remainder = (4 - (size % 4)) % 4;

    return spc_read(reader, size + remainder);
}

char* spc_read_str(reader_t* reader)
{
    unsigned char* end = memchr(reader->ptr, 0, reader->end - reader->ptr);
    if (!end)
        return NULL;

    return spc_read_padded(reader, end - reader->ptr + 1);
}

spc_port_t spc_reader_next_port(reader_t* reader)
{
    if (reader->next_port >= reader->num_ports)
        return SPC_NULL_PORT;

    reader->next_port++;
    return reader->ports[reader->next_port - 1];
}

spc_value_t spc_deserialize_value(reader_t* reader);

spc_array_t* spc_deserialize_array(reader_t* reader)
{
    spc_array_t* array = spc_array_create();
    spc_read_uint32(reader);
    size_t length = spc_read_uint32(reader);
    for (uint32_t i = 0; i < length; i++) {
        spc_value_t value = spc_deserialize_value(reader);
        spc_array_set_value(array, i, value);
    }

    return array;
}

spc_dictionary_t* spc_deserialize_dict(reader_t* reader)
{
    spc_dictionary_t* dict = spc_dictionary_create();
    spc_read_uint32(reader);
    dict->num_items = spc_read_uint32(reader);
    for (uint32_t i = 0; i < dict->num_items; i++) {
        spc_dictionary_item_t* item = malloc(sizeof(spc_dictionary_item_t));
        item->key = strdup(spc_read_str(reader));
        item->value = spc_deserialize_value(reader);
        item->next = dict->items;
        dict->items = item;
    }

    return dict;
}

spc_value_t spc_deserialize_value(reader_t* reader)
{
    spc_value_t value;
    value.type = spc_read_uint32(reader);
    switch (value.type) {
        case SPC_TYPE_NULL:
            break;
        case SPC_TYPE_BOOL:
            value.value.u64 = spc_read_uint32(reader);
            break;
        case SPC_TYPE_UINT64:
            value.value.u64 = spc_read_uint64(reader);
            break;
        case SPC_TYPE_INT64:
            value.value.i64 = spc_read_int64(reader);
            break;
        case SPC_TYPE_DOUBLE:
            value.value.dbl = spc_read_double(reader);
            break;
        case SPC_TYPE_STRING:
            spc_read_uint32(reader);
            value.value.str = strdup(spc_read_str(reader));
            break;
        case SPC_TYPE_ARRAY:
            value.value.array = spc_deserialize_array(reader);
            break;
        case SPC_TYPE_DICT:
            value.value.dict = spc_deserialize_dict(reader);
            break;
        case SPC_TYPE_SEND_PORT:
            value.value.port = spc_reader_next_port(reader);
            break;
        case SPC_TYPE_RECV_PORT:
            value.value.port = spc_reader_next_port(reader);
            break;
        case SPC_TYPE_UUID:
            value.value.ptr = malloc(0x10);
            memcpy(value.value.ptr, spc_read(reader, 0x10), 0x10);
            break;
        case SPC_TYPE_DATA:
            value.value.data.size = spc_read_uint32(reader);
            value.value.data.ptr = malloc(value.value.data.size);
            memcpy(value.value.data.ptr, spc_read_padded(reader, value.value.data.size), value.value.data.size);
            break;
        default:
            printf("Unsupported value type: 0x%x\n", value.type);
            exit(-1);
    }

    return value;
}

#define MSGID_CONNECTION_INTERRUPTED 71

spc_message_t* spc_deserialize(spc_mach_message_t* mach_msg)
{
    reader_t reader;
    reader.next_port = 0;
    reader.num_ports = 0;
    reader.ports = NULL;
    reader.end = (unsigned char*)mach_msg + mach_msg->header.msgh_size;
    reader.ptr = mach_msg->buf;

    // Handle well-known message IDs
    if (mach_msg->header.msgh_id == MSGID_CONNECTION_INTERRUPTED) {
        spc_dictionary_t* dict = spc_dictionary_create();
        spc_dictionary_set_string(dict, "error", "Connection interrupted");
        printf("Connection interrupted\n");
        // TODO
        exit(-1);
    }

    if (mach_msg->header.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
        mach_msg_body_t* body = (mach_msg_body_t*)spc_read(&reader, sizeof(mach_msg_body_t));
        for (int i = 0; i < body->msgh_descriptor_count; i++) {
            mach_msg_descriptor_type_t type = ((mach_msg_type_descriptor_t*)reader.ptr)->type;
            switch (type) {
                case MACH_MSG_PORT_DESCRIPTOR: {
                    reader.ports = realloc(reader.ports, (reader.num_ports + 1) * sizeof(spc_port_t));
                    mach_msg_port_descriptor_t* descriptor = (mach_msg_port_descriptor_t*)spc_read(&reader, sizeof(mach_msg_port_descriptor_t));
                    reader.ports[reader.num_ports].name = descriptor->name;
                    reader.ports[reader.num_ports].type = descriptor->disposition;
                    reader.num_ports += 1;
                    break;
                }
                case MACH_MSG_OOL_DESCRIPTOR:
                    spc_read(&reader, sizeof(mach_msg_ool_descriptor_t));
                    printf("Warning: ignoring OOL descriptor\n");
                    break;
                case MACH_MSG_OOL_PORTS_DESCRIPTOR:
                    spc_read(&reader, sizeof(mach_msg_ool_ports_descriptor_t));
                    printf("Warning: ignoring OOL ports descriptor\n");
                    break;
                default:
                    printf("Unsupported mach message descriptor type: %d\n", type);
                    exit(-1);
            }
        }
    }

    if (memcmp(spc_read(&reader, 8), "CPX@\x05\x00\x00\x00", 8) != 0) {
        puts("Invalid XPC message header");
        return NULL;
    }

    spc_value_t value = spc_deserialize_value(&reader);
    if (value.type != SPC_TYPE_DICT) {
        spc_value_destroy(value);
        puts("Invalid XPC message type");
        return NULL;
    }

    spc_message_t* msg = malloc(sizeof(spc_message_t));
    msg->remote_port.name = mach_msg->header.msgh_remote_port;
    msg->remote_port.type = MACH_MSGH_BITS_REMOTE(mach_msg->header.msgh_bits);
    msg->local_port.name = mach_msg->header.msgh_remote_port;
    msg->local_port.type = MACH_MSGH_BITS_LOCAL(mach_msg->header.msgh_bits);
    msg->id = mach_msg->header.msgh_id;
    msg->content = value.value.dict;

    return msg;
}
