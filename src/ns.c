
/*
 * Lux ACPI Implementation
 * Copyright (C) 2018-2019 the lai authors
 */

/* Generic ACPI Namespace Management */

#include <lai/core.h>
#include "aml_opcodes.h"
#include "ns_impl.h"
#include "exec_impl.h"
#include "libc.h"
#include "eval.h"

#define CODE_WINDOW            131072
#define NAMESPACE_WINDOW       8192

int lai_do_osi_method(lai_object_t *args, lai_object_t *result);
int lai_do_os_method(lai_object_t *args, lai_object_t *result);
int lai_do_rev_method(lai_object_t *args, lai_object_t *result);

// These variables are information about the size, capacity,
// and number of objects in the buffer we use to store all
// the AML objects.
uint8_t *lai_aml_code;
size_t lai_aml_capacity = 0;
size_t lai_aml_size = 0;
size_t lai_aml_obj_count = 0;
extern char aml_test[];

lai_nsnode_t **lai_namespace;
size_t lai_ns_size = 0;
size_t lai_ns_capacity = 0;

void lai_load_table(void *);

lai_nsnode_t *lai_create_nsnode(void) {
    lai_nsnode_t *node = laihost_malloc(sizeof(lai_nsnode_t));
    if (!node)
        return NULL;
    // here we assume that the host does not return zeroed memory,
    // so lai must zero the returned memory itself.
    memset(node, 0, sizeof(lai_nsnode_t));
    return node;
}

lai_nsnode_t *lai_create_nsnode_or_die(void) {
    lai_nsnode_t *node = lai_create_nsnode();
    if (!node)
        lai_panic("could not allocate new namespace node");
    return node;
}

// Installs the nsnode to the namespace.
void lai_install_nsnode(lai_nsnode_t *node) {
    if (lai_ns_size == lai_ns_capacity) {
        size_t new_capacity = lai_ns_capacity * 2;
        if (!new_capacity)
            new_capacity = NAMESPACE_WINDOW;
        lai_nsnode_t **new_array;
        new_array = laihost_realloc(lai_namespace, sizeof(lai_nsnode_t *) * new_capacity);
        if (!new_array)
            lai_panic("could not reallocate namespace table");
        lai_namespace = new_array;
        lai_ns_capacity = new_capacity;
    }

    /*lai_debug("created %s\n", node->path);*/
    lai_namespace[lai_ns_size++] = node;
}

size_t lai_resolve_path(lai_nsnode_t *context, char *fullpath, uint8_t *path) {
    size_t name_size = 0;
    size_t multi_count = 0;
    size_t current_count = 0;

    memset(fullpath, 0, ACPI_MAX_NAME);

    if (path[0] == ROOT_CHAR) {
        name_size = 1;
        fullpath[0] = ROOT_CHAR;
        fullpath[1] = 0;
        path++;
        if (lai_is_name(path[0]) || path[0] == DUAL_PREFIX || path[0] == MULTI_PREFIX) {
            fullpath[1] = '.';
            fullpath[2] = 0;
            goto start;
        } else
            return name_size;
    }

    if (context)
        lai_strcpy(fullpath, context->path);
    else
        lai_strcpy(fullpath, "\\");
    fullpath[lai_strlen(fullpath)] = '.';

start:
    while(path[0] == PARENT_CHAR) {
        path++;
        if (lai_strlen(fullpath) <= 2)
            break;

        name_size++;
        fullpath[lai_strlen(fullpath) - 5] = 0;
        memset(fullpath + lai_strlen(fullpath), 0, 32);
    }

    if (path[0] == DUAL_PREFIX) {
        name_size += 9;
        path++;
        memcpy(fullpath + lai_strlen(fullpath), path, 4);
        fullpath[lai_strlen(fullpath)] = '.';
        memcpy(fullpath + lai_strlen(fullpath), path + 4, 4);
    } else if (path[0] == MULTI_PREFIX) {
        // skip MULTI_PREFIX and name count
        name_size += 2;
        path++;

        // get name count here
        multi_count = (size_t)path[0];
        path++;

        current_count = 0;
        while (current_count < multi_count) {
            name_size += 4;
            memcpy(fullpath + lai_strlen(fullpath), path, 4);
            path += 4;
            current_count++;
            if (current_count >= multi_count)
                break;

            fullpath[lai_strlen(fullpath)] = '.';
        }
    } else {
        name_size += 4;
        memcpy(fullpath + lai_strlen(fullpath), path, 4);
    }

    return name_size;
}

// Creates the ACPI namespace. Requires the ability to scan for ACPI tables - ensure this is
// implemented in the host operating system.
void lai_create_namespace(void) {
    if (!laihost_scan)
        lai_panic("lai_create_namespace() needs table management functions");

    lai_namespace = lai_calloc(sizeof(lai_nsnode_t *), NAMESPACE_WINDOW);
    if (!lai_namespace)
        lai_panic("unable to allocate memory.");

    lai_aml_code = laihost_malloc(CODE_WINDOW);
    lai_aml_capacity = CODE_WINDOW;

    //acpins_load_table(aml_test);    // custom AML table just for testing

    // we need the FADT
    lai_fadt = laihost_scan("FACP", 0);
    if (!lai_fadt) {
        lai_panic("unable to find ACPI FADT.");
    }

    void *dsdt = laihost_scan("DSDT", 0);
    lai_load_table(dsdt);

    // load all SSDTs
    size_t index = 0;
    acpi_aml_t *ssdt = laihost_scan("SSDT", index);
    while (ssdt != NULL) {
        lai_load_table(ssdt);
        index++;
        ssdt = laihost_scan("SSDT", index);
    }

    // the PSDT is treated the same way as the SSDT
    // scan for PSDTs too for compatibility with some ACPI 1.0 PCs
    index = 0;
    acpi_aml_t *psdt = laihost_scan("PSDT", index);
    while (psdt != NULL) {
        lai_load_table(psdt);
        index++;
        psdt = laihost_scan("PSDT", index);
    }

    // create the OS-defined objects first
    lai_nsnode_t *osi_node = lai_create_nsnode_or_die();
    osi_node->type = LAI_NAMESPACE_METHOD;
    lai_strcpy(osi_node->path, "\\._OSI");
    osi_node->method_flags = 0x01;
    osi_node->method_override = &lai_do_osi_method;
    lai_install_nsnode(osi_node);

    lai_nsnode_t *os_node = lai_create_nsnode_or_die();
    os_node->type = LAI_NAMESPACE_METHOD;
    lai_strcpy(os_node->path, "\\._OS_");
    os_node->method_flags = 0x00;
    os_node->method_override = &lai_do_os_method;
    lai_install_nsnode(os_node);

    lai_nsnode_t *rev_node = lai_create_nsnode_or_die();
    rev_node->type = LAI_NAMESPACE_METHOD;
    lai_strcpy(rev_node->path, "\\._REV");
    rev_node->method_flags = 0x00;
    rev_node->method_override = &lai_do_rev_method;
    lai_install_nsnode(rev_node);

    // Create the namespace with all the objects.
    lai_state_t state;
    lai_init_state(&state);
    lai_populate(NULL, lai_aml_code, lai_aml_size, &state);
    lai_finalize_state(&state);

    lai_debug("ACPI namespace created, total of %d predefined objects.", lai_ns_size);
}

void lai_load_table(void *ptr) {
    acpi_aml_t *table = (acpi_aml_t*)ptr;
    while (lai_aml_size + table->header.length >= lai_aml_capacity) {
        lai_aml_capacity += CODE_WINDOW;
        lai_aml_code = laihost_realloc(lai_aml_code, lai_aml_capacity);
    }

    // copy the actual AML code
    memcpy(lai_aml_code + lai_aml_size, table->data, table->header.length - sizeof(acpi_header_t));
    lai_aml_size += (table->header.length - sizeof(acpi_header_t));

    lai_debug("loaded AML table '%c%c%c%c', total %d bytes of AML code.", table->header.signature[0], table->header.signature[1], table->header.signature[2], table->header.signature[3], lai_aml_size);

    lai_aml_obj_count++;
}

// TODO: This entire function could probably do with a rewrite soonish.
size_t lai_create_field(lai_nsnode_t *parent, void *data) {
    uint8_t *field = (uint8_t *)data;
    field += 2;        // skip opcode

    // package size
    size_t pkgsize, size;

    pkgsize = lai_parse_pkgsize(field, &size);
    field += pkgsize;

    // determine name of opregion
    lai_nsnode_t *opregion;
    char opregion_name[ACPI_MAX_NAME];
    size_t name_size = 0;

    name_size = lai_resolve_path(parent, opregion_name, field);

    opregion = lai_exec_resolve(opregion_name);
    if (!opregion) {
        lai_debug("error parsing field for non-existant OpRegion %s, ignoring...", opregion_name);
        return size + 2;
    }

    // parse the field's entries now
    uint8_t field_flags;
    field = (uint8_t *)data + 2 + pkgsize + name_size;
    field_flags = field[0];


    // FIXME: Why this increment_namespace()?
    //acpins_increment_namespace();

    field++;        // actual field objects
    size_t byte_count = (size_t)((size_t)field - (size_t)data);

    uint64_t current_offset = 0;
    size_t skip_size, skip_bits;
    size_t field_size;

    while (byte_count < size) {
        if (!field[0]) {
            field++;
            byte_count++;

            skip_size = lai_parse_pkgsize(field, &skip_bits);
            current_offset += skip_bits;

            field += skip_size;
            byte_count += skip_size;

            continue;
        }

        if (field[0] == 1) {
            field_flags = field[1];

            field += 3;
            byte_count += 3;

            continue;
        }

        if(field[0] == 2) {
            lai_warn("field for OpRegion %s: ConnectField unimplemented.", opregion->path);

            field++;
            byte_count++;

            while (!lai_is_name(field[0])) {
                field++;
                byte_count++;
            }
        }

        if (byte_count >= size)
            break;

        lai_nsnode_t *node = lai_create_nsnode_or_die();
        node->type = LAI_NAMESPACE_FIELD;

        name_size = lai_resolve_path(parent, node->path, &field[0]);
        field += name_size;
        byte_count += name_size;

        // FIXME: This looks odd. Why do we insert a dot in the middle of the path?
        /*node->path[lai_strlen(parent->path)] = '.';*/

        lai_strcpy(node->field_opregion, opregion->path);

        field_size = lai_parse_pkgsize(&field[0], &node->field_size);

        node->field_flags = field_flags;
        node->field_offset = current_offset;

        current_offset += (uint64_t)node->field_size;
        lai_install_nsnode(node);

        field += field_size;
        byte_count += field_size;
    }

    return size + 2;
}

// Create a control method in the namespace.
size_t lai_create_method(lai_nsnode_t *parent, void *data) {
    uint8_t *method = (uint8_t *)data;
    method++;        // skip over METHOD_OP

    size_t size, pkgsize;
    pkgsize = lai_parse_pkgsize(method, &size);
    method += pkgsize;

    // create a namespace object for the method
    lai_nsnode_t *node = lai_create_nsnode_or_die();
    size_t name_length = lai_resolve_path(parent, node->path, method);

    // get the method's flags
    method = (uint8_t *)data;
    method += pkgsize + name_length + 1;

    // create a node corresponding to this method,
    // and add it to the namespace.
    node->type = LAI_NAMESPACE_METHOD;
    node->method_flags = method[0];
    node->pointer = (void *)(method + 1);
    node->size = size - pkgsize - name_length - 1;

    lai_install_nsnode(node);
    return size + 1;
}

// Create an alias in the namespace
size_t lai_create_alias(lai_nsnode_t *parent, void *data) {
    size_t return_size = 1;
    uint8_t *alias = (uint8_t *)data;
    alias++;        // skip ALIAS_OP

    size_t name_size;

    lai_nsnode_t *node = lai_create_nsnode_or_die();
    node->type = LAI_NAMESPACE_ALIAS;
    name_size = lai_resolve_path(parent, node->alias, alias);

    return_size += name_size;
    alias += name_size;

    name_size = lai_resolve_path(parent, node->path, alias);

    //lai_debug("alias %s for object %s\n", node->path, node->alias);

    lai_install_nsnode(node);
    return_size += name_size;
    return return_size;
}

size_t lai_create_mutex(lai_nsnode_t *parent, void *data) {
    size_t return_size = 2;
    uint8_t *mutex = (uint8_t *)data;
    mutex += 2;        // skip MUTEX_OP

    lai_nsnode_t *node = lai_create_nsnode_or_die();
    node->type = LAI_NAMESPACE_MUTEX;
    size_t name_size = lai_resolve_path(parent, node->path, mutex);

    return_size += name_size;
    return_size++;

    lai_install_nsnode(node);
    return return_size;
}

size_t lai_create_indexfield(lai_nsnode_t *parent, void *data) {
    uint8_t *indexfield = (uint8_t *)data;
    indexfield += 2;        // skip INDEXFIELD_OP

    size_t pkgsize, size;
    pkgsize = lai_parse_pkgsize(indexfield, &size);

    indexfield += pkgsize;

    // index and data
    char indexr[ACPI_MAX_NAME], datar[ACPI_MAX_NAME];
    memset(indexr, 0, ACPI_MAX_NAME);
    memset(datar, 0, ACPI_MAX_NAME);

    indexfield += lai_resolve_path(parent, indexr, indexfield);
    indexfield += lai_resolve_path(parent, datar, indexfield);

    uint8_t flags = indexfield[0];

    indexfield++;            // actual field list
    size_t byte_count = (size_t)((size_t)indexfield - (size_t)data);

    uint64_t current_offset = 0;
    size_t skip_size, skip_bits;
    size_t name_size;

    while (byte_count < size) {
        while (!indexfield[0]) {
            indexfield++;
            byte_count++;

            skip_size = lai_parse_pkgsize(indexfield, &skip_bits);
            current_offset += skip_bits;

            indexfield += skip_size;
            byte_count += skip_size;
        }

        //lai_debug("indexfield %c%c%c%c: size %d bits, at bit offset %d\n", indexfield[0], indexfield[1], indexfield[2], indexfield[3], indexfield[4], current_offset);
        lai_nsnode_t *node = lai_create_nsnode_or_die();
        node->type = LAI_NAMESPACE_INDEXFIELD;
        // FIXME: This looks odd. Why don't we all acpins_resolve_path()?

        /*memcpy(node->path, parent->path, lai_strlen(parent->path));
        node->path[lai_strlen(parent->path)] = '.';
        memcpy(node->path + lai_strlen(parent->path) + 1, indexfield, 4);*/

        name_size = lai_resolve_path(parent, node->path, &indexfield[0]);

        indexfield += name_size;
        byte_count += name_size;

        lai_strcpy(node->indexfield_data, datar);
        lai_strcpy(node->indexfield_index, indexr);

        node->indexfield_flags = flags;
        node->indexfield_size = indexfield[0];
        node->indexfield_offset = current_offset;

        current_offset += (uint64_t)(indexfield[0]);
        lai_install_nsnode(node);

        indexfield++;
        byte_count++;
    }

    return size + 2;
}

// acpins_create_processor(): Creates a Processor object in the namespace
// Param:    void *data - pointer to data
// Return:    size_t - total size in bytes, for skipping

size_t lai_create_processor(lai_nsnode_t *parent, void *data) {
    uint8_t *processor = (uint8_t *)data;
    processor += 2;            // skip over PROCESSOR_OP

    size_t pkgsize, size;
    pkgsize = lai_parse_pkgsize(processor, &size);
    processor += pkgsize;

    lai_nsnode_t *node = lai_create_nsnode_or_die();
    node->type = LAI_NAMESPACE_PROCESSOR;
    size_t name_size = lai_resolve_path(parent, node->path, processor);
    processor += name_size;

    node->cpu_id = processor[0];

    lai_install_nsnode(node);

    return size + 2;
}

// creates an object of type such as DWORDFIELD or QWORDFIELD, where the value
// n corresponds to the width of the type in question; for example, for a DWORDFIELD,
// n =32.
size_t lai_create_n_wordfield(lai_nsnode_t *parent, void *data, size_t n) {
    uint8_t *field = (uint8_t *)data;
    // skip over the op prefix.
    field++;
    size_t size = 1;

    lai_nsnode_t *node = lai_create_nsnode_or_die();
    node->type = LAI_NAMESPACE_BUFFER_FIELD;

    // buffer name
    size_t name_size = lai_resolve_path(parent, node->buffer, field);

    size += name_size;
    field += name_size;

    uint64_t integer;
    size_t integer_size = lai_eval_integer(field, &integer);

    node->buffer_offset = integer * 8;
    // the buffer is as wide as the width of the word
    node->buffer_size = n;

    size += integer_size;
    field += integer_size;

    name_size = lai_resolve_path(parent, node->path, field);

    lai_install_nsnode(node);
    size += name_size;
    return size;
}

// Resolve a namespace object by its path
lai_nsnode_t *lai_resolve(char *path) {
    size_t i = 0;

    if (path[0] == ROOT_CHAR) {
        while(i < lai_ns_size) {
            if(!lai_strcmp(lai_namespace[i]->path, path))
                return lai_namespace[i];
            else
                i++;
        }

        return NULL;
    } else {
        while (i < lai_ns_size) {
            if (!memcmp(lai_namespace[i]->path + lai_strlen(lai_namespace[i]->path) - 4, path, 4))
                return lai_namespace[i];
            else
                i++;
        }

        return NULL;
    }
}

// search for a device by its index
lai_nsnode_t *lai_get_device(size_t index) {
    size_t i = 0, j = 0;
    while (j < lai_ns_size) {
        if (lai_namespace[j]->type == LAI_NAMESPACE_DEVICE)
            i++;

        if (i > index)
            return lai_namespace[j];

        j++;
    }

    return NULL;
}

// search for a device by its id and index.
lai_nsnode_t *lai_get_deviceid(size_t index, lai_object_t *id) {
    size_t i = 0, j = 0;

    lai_nsnode_t *handle;
    char path[ACPI_MAX_NAME];
    lai_object_t device_id = {0};

    handle = lai_get_device(j);
    while (handle) {
        // read the ID of the device
        lai_strcpy(path, handle->path);
        // change the device ID to hardware ID
        lai_strcpy(path + lai_strlen(path), "._HID");
        memset(&device_id, 0, sizeof(lai_object_t));
        if (lai_eval(&device_id, path)) {
            // same principle here
            lai_strcpy(path + lai_strlen(path) - 5, "._CID");
            memset(&device_id, 0, sizeof(lai_object_t));
            lai_eval(&device_id, path);
        }

        if (device_id.type == LAI_INTEGER && id->type == LAI_INTEGER) {
            if (device_id.integer == id->integer)
                i++;
        } else if (device_id.type == LAI_STRING && id->type == LAI_STRING) {
            if (!lai_strcmp(device_id.string, id->string))
                i++;
        }

        if (i > index)
            return handle;

        j++;
        handle = lai_get_device(j);
    }

    return NULL;
}

// determine the node in the ACPI namespace corresponding to a given path,
// and return this node.
lai_nsnode_t *lai_enum(char *parent, size_t index) {
    index++;
    size_t parent_size = lai_strlen(parent);
    for (size_t i = 0; i < lai_ns_size; i++) {
        if (!memcmp(parent, lai_namespace[i]->path, parent_size)) {
            if(!index)
                return lai_namespace[i];
            else
                index--;
        }
    }

    return NULL;
}
