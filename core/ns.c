
/*
 * Lightweight ACPI Implementation
 * Copyright (C) 2018-2019 the lai authors
 */

/* Generic ACPI Namespace Management */

#include <lai/core.h>
#include "aml_opcodes.h"
#include "ns_impl.h"
#include "exec_impl.h"
#include "libc.h"
#include "eval.h"
#include "util-hash.h"

#define CODE_WINDOW            131072
#define NAMESPACE_WINDOW       8192

static int debug_namespace = 0;
static int debug_resolution = 0;

int lai_do_osi_method(lai_variable_t *args, lai_variable_t *result);
int lai_do_os_method(lai_variable_t *args, lai_variable_t *result);
int lai_do_rev_method(lai_variable_t *args, lai_variable_t *result);

lai_nsnode_t *lai_root_node;
lai_nsnode_t **lai_namespace;
size_t lai_ns_size = 0;
size_t lai_ns_capacity = 0;

acpi_fadt_t *lai_fadt;

static struct lai_aml_segment *lai_load_table(void *ptr, int index);

static unsigned int lai_hash_string(const char *str, size_t n) {
    // Simple djb2 hash function. TODO: Replace by SipHash for DoS resilience.
    unsigned int x = 5381;
    for (size_t i = 0; i < n; i++)
        x = ((x << 5) + x) + str[i];
    return x;
}

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
    if (debug_namespace) {
        LAI_CLEANUP_FREE_STRING char *fullpath = lai_stringify_node_path(node);
        lai_debug("created %s", fullpath);
    }

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

    lai_namespace[lai_ns_size++] = node;

    // Insert the node into its parent's hash table.
    lai_nsnode_t *parent = node->parent;
    if (parent) {
        int h = lai_hash_string(node->name, 4);

        struct lai_hashtable_chain chain = LAI_HASHTABLE_CHAIN_INITIALIZER;
        while (!lai_hashtable_chain_advance(&parent->children, h, &chain)) {
            lai_nsnode_t *child = lai_hashtable_chain_get(&parent->children, h, &chain);
            if (!memcmp(child->name, node->name, 4)) {
                LAI_CLEANUP_FREE_STRING char *fullpath = lai_stringify_node_path(node);
                lai_panic("trying to install duplicate namespace node %s", fullpath);
            }
        }

        lai_hashtable_insert(&parent->children, h, node);
    }
}

void lai_uninstall_nsnode(lai_nsnode_t *node) {
    for (size_t i = 0; i < lai_ns_size; i++) {
        if (lai_namespace[i] == node)
            lai_namespace[i] = NULL;
    }

    // Remove the node from its parent's hash table.
    lai_nsnode_t *parent = node->parent;
    if (parent) {
        int h = lai_hash_string(node->name, 4);
        struct lai_hashtable_chain chain = LAI_HASHTABLE_CHAIN_INITIALIZER;
        for (;;) {
            if (lai_hashtable_chain_advance(&parent->children, h, &chain))
                lai_panic("child node is missing from parent's hash table"
                          " during lai_uninstall_nsnode()");
            lai_nsnode_t *child = lai_hashtable_chain_get(&parent->children, h, &chain);
            if (child != node)
                continue;
            lai_hashtable_chain_remove(&parent->children, h, &chain);
            break;
        }

        // As a sanity-check: make sure that the child does not occur twice.
        while (!lai_hashtable_chain_advance(&parent->children, h, &chain)) {
            lai_nsnode_t *child = lai_hashtable_chain_get(&parent->children, h, &chain);
            if (child == node)
                lai_panic("child node appears multiple times in parent's hash table"
                          " during lai_uninstall_nsnode()");
        }
    }
}

lai_nsnode_t *lai_ns_get_root() {
    return lai_root_node;
}

lai_nsnode_t *lai_ns_get_parent(lai_nsnode_t *node) {
    return node->parent;
}

lai_nsnode_t *lai_ns_get_child(lai_nsnode_t *parent, const char *name) {
    int h = lai_hash_string(name, 4);
    struct lai_hashtable_chain chain = LAI_HASHTABLE_CHAIN_INITIALIZER;
    while (!lai_hashtable_chain_advance(&parent->children, h, &chain)) {
        lai_nsnode_t *child = lai_hashtable_chain_get(&parent->children, h, &chain);
        if (!memcmp(child->name, name, 4))
            return child;
    }
    return NULL;
}

size_t lai_amlname_parse(struct lai_amlname *amln, const void *data) {
    amln->is_absolute = 0;
    amln->height = 0;

    const uint8_t *begin = data;
    const uint8_t *it = begin;
    if (*it == '\\') {
        // First character is \ for absolute paths.
        amln->is_absolute = 1;
        it++;
    } else {
        // Non-absolute paths can be prefixed by a number of ^.
        while (*it == '^') {
            amln->height++;
            it++;
        }
    }

    // Finally, we parse the name's prefix (which determines the number of segments).
    int num_segs;
    if (*it == '\0') {
        it++;
        num_segs = 0;
    } else if (*it == DUAL_PREFIX) {
        it++;
        num_segs = 2;
    } else if (*it == MULTI_PREFIX) {
        it++;
        num_segs = *it;
        LAI_ENSURE(num_segs > 2);
        it++;
    } else {
        LAI_ENSURE(lai_is_name(*it));
        num_segs = 1;
    }

    amln->search_scopes = !amln->is_absolute && !amln->height && num_segs == 1;
    amln->it = it;
    amln->end = it + 4 * num_segs;
    return amln->end - begin;
}

int lai_amlname_done(struct lai_amlname *amln) {
    return amln->it == amln->end;
}

void lai_amlname_iterate(struct lai_amlname *amln, char *out) {
    LAI_ENSURE(amln->it < amln->end);
    for (int i = 0; i < 4; i++)
        out[i] = amln->it[i];
    amln->it += 4;
}

char *lai_stringify_amlname(const struct lai_amlname *in_amln) {
    // Make a copy to avoid rendering the original object unusable.
    struct lai_amlname amln = *in_amln;

    size_t num_segs = (amln.end - amln.it) / 4;
    size_t max_length = 1              // Leading \ for absolute paths.
                        + amln.height // Leading ^ characters.
                        + num_segs * 5 // Segments, seperated by dots.
                        + 1;          // Null-terminator.

    char *str = laihost_malloc(max_length);
    if (!str)
        lai_panic("could not allocate in lai_stringify_amlname()");

    int n = 0;
    if (amln.is_absolute)
        str[n++] = '\\';
    for (int i = 0; i < amln.height; i++)
        str[n++] = '^';

    if(!lai_amlname_done(&amln)) {
        for (;;) {
            lai_amlname_iterate(&amln, &str[n]);
            n += 4;
            if (lai_amlname_done(&amln))
                break;
            str[n++] = '.';
        }
    }
    str[n++] = '\0';
    LAI_ENSURE(n <= max_length);
    return str;
}

char *lai_stringify_node_path(lai_nsnode_t *node) {
    // Handle the trivial case.
    if (!node->parent) {
        LAI_ENSURE(node->type == LAI_NAMESPACE_ROOT);
        char *str = laihost_malloc(2);
        if (!str)
            lai_panic("could not allocate in lai_stringify_node_path()");
        lai_strcpy(str, "\\");
        return str;
    }

    lai_nsnode_t *current;

    // Find the number of segments, excluding the root.
    size_t num_segs = 0;
    for (current = node; current->parent; current = current->parent)
        num_segs++;

    size_t length = num_segs * 5; // Leading dot (or \) and four chars per segment.
    char *str = laihost_malloc(length + 1);
    if (!str)
        lai_panic("could not allocate in lai_stringify_node_path()");

    // Build the string from right to left.
    size_t n = length;
    for (current = node; current->parent; current = current->parent) {
        n -= 4;
        lai_namecpy(str + n, current->name);
        n -= 1;
        str[n] = '.';
    }
    LAI_ENSURE(!n);
    str[0] = '\\'; // Overwrites the first dot.
    str[length] = '\0';
    return str;
}

lai_nsnode_t *lai_do_resolve(lai_nsnode_t *ctx_handle, const struct lai_amlname *in_amln) {
    // Make a copy to avoid rendering the original object unusable.
    struct lai_amlname amln = *in_amln;

    lai_nsnode_t *current = ctx_handle;
    LAI_ENSURE(current);
    LAI_ENSURE(current->type != LAI_NAMESPACE_ALIAS); // ctx_handle needs to be resolved.

    if (amln.search_scopes) {
        char segment[5];
        lai_amlname_iterate(&amln, segment);
        LAI_ENSURE(lai_amlname_done(&amln));
        segment[4] = '\0';

        if(debug_resolution)
            lai_debug("resolving %s by searching through scopes", segment);

        while (current) {
            lai_nsnode_t *node = lai_ns_get_child(current, segment);
            if (!node) {
                current = current->parent;
                continue;
            }

            if (node->type == LAI_NAMESPACE_ALIAS) {
                node = node->al_target;
                LAI_ENSURE(node->type != LAI_NAMESPACE_ALIAS);
            }
            if(debug_resolution) {
                LAI_CLEANUP_FREE_STRING char *fullpath = lai_stringify_node_path(node);
                lai_debug("resolution returns %s", fullpath);
            }
            return node;
        }

        return NULL;
    } else {
        if (amln.is_absolute) {
            while (current->parent)
                current = current->parent;
            LAI_ENSURE(current->type == LAI_NAMESPACE_ROOT);
        }

        for (int i = 0; i < amln.height; i++) {
            if (!current->parent) {
                LAI_ENSURE(current->type == LAI_NAMESPACE_ROOT);
                break;
            }
            current = current->parent;
        }

        if (lai_amlname_done(&amln))
            return current;

        while (!lai_amlname_done(&amln)) {
            char segment[4];
            lai_amlname_iterate(&amln, segment);
            current = lai_ns_get_child(current, segment);
            if (!current)
                return NULL;
        }

        if (current->type == LAI_NAMESPACE_ALIAS) {
            current = current->al_target;
            LAI_ENSURE(current->type != LAI_NAMESPACE_ALIAS);
        }
        return current;
    }
}

void lai_do_resolve_new_node(lai_nsnode_t *node,
        lai_nsnode_t *ctx_handle, const struct lai_amlname *in_amln) {
    // Make a copy to avoid rendering the original object unusable.
    struct lai_amlname amln = *in_amln;

    lai_nsnode_t *parent = ctx_handle;
    LAI_ENSURE(parent);
    LAI_ENSURE(parent->type != LAI_NAMESPACE_ALIAS); // ctx_handle needs to be resolved.

    // Note: we do not care about amln->search_scopes here.
    //       As we are creating a new name, the code below already does the correct thing.

    if (amln.is_absolute) {
        while (parent->parent)
            parent = parent->parent;
        LAI_ENSURE(parent->type == LAI_NAMESPACE_ROOT);
    }

    for (int i = 0; i < amln.height; i++) {
        if (!parent->parent) {
            LAI_ENSURE(parent->type == LAI_NAMESPACE_ROOT);
            break;
        }
        parent = parent->parent;
    }

    // Otherwise the new object has an empty name.
    LAI_ENSURE(!lai_amlname_done(&amln));

    for (;;) {
        char segment[5];
        lai_amlname_iterate(&amln, segment);
        segment[4] = '\0';

        if (lai_amlname_done(&amln)) {
            // The last segment is the name of the new node.
            lai_namecpy(node->name, segment);
            node->parent = parent;
            break;
        } else {
            parent = lai_ns_get_child(parent, segment);
            LAI_ENSURE(parent);
            if (parent->type == LAI_NAMESPACE_ALIAS) {
                lai_warn("resolution of new object name traverses Alias(),"
                        " this is not supported in ACPICA");
                parent = parent->al_target;
                LAI_ENSURE(parent->type != LAI_NAMESPACE_ALIAS);
            }
        }
    }
}

size_t lai_resolve_new_node(lai_nsnode_t *node, lai_nsnode_t *ctx_handle, void *data) {
    struct lai_amlname amln;
    size_t size = lai_amlname_parse(&amln, data);
    lai_do_resolve_new_node(node, ctx_handle, &amln);
    return size;
}

lai_nsnode_t *lai_create_root(void) {
    lai_root_node = lai_create_nsnode_or_die();
    lai_root_node->type = LAI_NAMESPACE_ROOT;
    lai_namecpy(lai_root_node->name, "\\___");
    lai_root_node->parent = NULL;

    // Create the predefined objects.
    lai_nsnode_t *sb_node = lai_create_nsnode_or_die();
    sb_node->type = LAI_NAMESPACE_DEVICE;
    lai_namecpy(sb_node->name, "_SB_");
    sb_node->parent = lai_root_node;
    lai_install_nsnode(sb_node);

    lai_nsnode_t *si_node = lai_create_nsnode_or_die();
    si_node->type = LAI_NAMESPACE_DEVICE;
    lai_namecpy(si_node->name, "_SI_");
    si_node->parent = lai_root_node;
    lai_install_nsnode(si_node);

    lai_nsnode_t *gpe_node = lai_create_nsnode_or_die();
    gpe_node->type = LAI_NAMESPACE_DEVICE;
    lai_namecpy(gpe_node->name, "_GPE");
    gpe_node->parent = lai_root_node;
    lai_install_nsnode(gpe_node);

    // Create nodes for compatibility with ACPI 1.0.
    lai_nsnode_t *pr_node = lai_create_nsnode_or_die();
    pr_node->type = LAI_NAMESPACE_DEVICE;
    lai_namecpy(pr_node->name, "_PR_");
    pr_node->parent = lai_root_node;
    lai_install_nsnode(pr_node);

    lai_nsnode_t *tz_node = lai_create_nsnode_or_die();
    tz_node->type = LAI_NAMESPACE_DEVICE;
    lai_namecpy(tz_node->name, "_TZ_");
    tz_node->parent = lai_root_node;
    lai_install_nsnode(tz_node);

    // Create the OS-defined objects.
    lai_nsnode_t *osi_node = lai_create_nsnode_or_die();
    osi_node->type = LAI_NAMESPACE_METHOD;
    lai_namecpy(osi_node->name, "_OSI");
    osi_node->parent = lai_root_node;
    osi_node->method_flags = 0x01;
    osi_node->method_override = &lai_do_osi_method;
    lai_install_nsnode(osi_node);

    lai_nsnode_t *os_node = lai_create_nsnode_or_die();
    os_node->type = LAI_NAMESPACE_METHOD;
    lai_namecpy(os_node->name, "_OS_");
    os_node->parent = lai_root_node;
    os_node->method_flags = 0x00;
    os_node->method_override = &lai_do_os_method;
    lai_install_nsnode(os_node);

    lai_nsnode_t *rev_node = lai_create_nsnode_or_die();
    rev_node->type = LAI_NAMESPACE_METHOD;
    lai_namecpy(rev_node->name, "_REV");
    rev_node->parent = lai_root_node;
    rev_node->method_flags = 0x00;
    rev_node->method_override = &lai_do_rev_method;
    lai_install_nsnode(rev_node);

    return lai_root_node;
}

// Creates the ACPI namespace. Requires the ability to scan for ACPI tables - ensure this is
// implemented in the host operating system.
void lai_create_namespace(void) {
    if (!laihost_scan)
        lai_panic("lai_create_namespace() needs table management functions");

    lai_namespace = lai_calloc(sizeof(lai_nsnode_t *), NAMESPACE_WINDOW);
    if (!lai_namespace)
        lai_panic("unable to allocate memory.");

    // we need the FADT
    lai_fadt = laihost_scan("FACP", 0);
    if (!lai_fadt) {
        lai_panic("unable to find ACPI FADT.");
    }

    lai_nsnode_t *root_node = lai_create_root();

    // Create the namespace with all the objects.
    lai_state_t state;

    // Load the DSDT.
    void *dsdt_table = laihost_scan("DSDT", 0);
    void *dsdt_amls = lai_load_table(dsdt_table, 0);
    lai_init_state(&state);
    lai_populate(root_node, dsdt_amls, &state);
    lai_finalize_state(&state);

    // Load all SSDTs.
    size_t index = 0;
    acpi_aml_t *ssdt_table;
    while ((ssdt_table = laihost_scan("SSDT", index))) {
        void *ssdt_amls = lai_load_table(ssdt_table, index);
        lai_init_state(&state);
        lai_populate(root_node, ssdt_amls, &state);
        lai_finalize_state(&state);
        index++;
    }

    // The PSDT is treated the same way as the SSDT.
    // Scan for PSDTs too for compatibility with some ACPI 1.0 PCs.
    index = 0;
    acpi_aml_t *psdt_table;
    while ((psdt_table = laihost_scan("PSDT", index))) {
        void *psdt_amls = lai_load_table(psdt_table, index);
        lai_init_state(&state);
        lai_populate(root_node, psdt_amls, &state);
        lai_finalize_state(&state);
        index++;
    }

    lai_debug("ACPI namespace created, total of %d predefined objects.", lai_ns_size);
}

static struct lai_aml_segment *lai_load_table(void *ptr, int index) {
    struct lai_aml_segment *amls = laihost_malloc(sizeof(struct lai_aml_segment));
    if(!amls)
        lai_panic("could not allocate memory for struct lai_aml_segment");
    memset(amls, 0, sizeof(struct lai_aml_segment));

    amls->table = ptr;
    amls->index = index;

    lai_debug("loaded AML table '%c%c%c%c', total %d bytes of AML code.",
            amls->table->header.signature[0],
            amls->table->header.signature[1],
            amls->table->header.signature[2],
            amls->table->header.signature[3],
            amls->table->header.length);
    return amls;
}

lai_nsnode_t *lai_resolve_path(lai_nsnode_t *ctx_handle, const char *path) {
    lai_nsnode_t *current = ctx_handle;
    if (!current)
        current = lai_root_node;

    if (*path == '\\') {
        while (current->parent)
            current = current->parent;
        LAI_ENSURE(current->type == LAI_NAMESPACE_ROOT);
        path++;
    } else {
        int height = 0;
        while (*path == '^') {
            height++;
            path++;
        }

        for (int i = 0; i < height; i++) {
            if (!current->parent) {
                LAI_ENSURE(current->type == LAI_NAMESPACE_ROOT);
                break;
            }
            current = current->parent;
        }
    }

    if (!(*path))
        return current;

    for(;;) {
        char segment[5];

        int k;
        for (k = 0; k < 4; k++) {
            if (!lai_is_name(*path))
                break;
            segment[k] = *(path++);
        }

        // ACPI pads names with trailing underscores.
        while (k < 4)
            segment[k++] = '_';
        segment[4] = '\0';

        current = lai_ns_get_child(current, segment);
        if (!current)
            return NULL;
        if (current->type == LAI_NAMESPACE_ALIAS) {
            current = current->al_target;
            LAI_ENSURE(current->type != LAI_NAMESPACE_ALIAS);
        }

        if (!(*path))
            break;
        LAI_ENSURE(*path == '.');
        path++;
    }

    return current;
}

lai_nsnode_t *lai_resolve_search(lai_nsnode_t *ctx_handle, const char *segment) {
    lai_nsnode_t *current = ctx_handle;
    LAI_ENSURE(current);
    LAI_ENSURE(current->type != LAI_NAMESPACE_ALIAS); // ctx_handle needs to be resolved.

    if(debug_resolution)
        lai_debug("resolving %s by searching through scopes", segment);

    while (current) {
        lai_nsnode_t *node = lai_ns_get_child(current, segment);
        if (!node) {
            current = current->parent;
            continue;
        }

        if (node->type == LAI_NAMESPACE_ALIAS) {
            node = node->al_target;
            LAI_ENSURE(node->type != LAI_NAMESPACE_ALIAS);
        }
        if (debug_resolution) {
            LAI_CLEANUP_FREE_STRING char *fullpath = lai_stringify_node_path(node);
            lai_debug("resolution returns %s", fullpath);
        }
        return node;
    }

    return NULL;
}

int lai_check_device_pnp_id(lai_nsnode_t *dev, lai_variable_t *pnp_id,
                            lai_state_t *state) {

    lai_variable_t id = {0};
    int ret = 1;

    lai_nsnode_t *hid_handle = lai_resolve_path(dev, "_HID");
    if (hid_handle) {
        if (lai_eval(&id, hid_handle, state)) {
            lai_warn("could not evaluate _HID of device");
        } else {
            LAI_ENSURE(id.type);
        }
    }

    if (!id.type) {
        lai_nsnode_t *cid_handle = lai_resolve_path(dev, "_CID");
        if (cid_handle) {
            if (lai_eval(&id, cid_handle, state)) {
                lai_warn("could not evaluate _CID of device");
                return 1;
            } else {
                LAI_ENSURE(id.type);
            }
        }
    }


    if (id.type == LAI_INTEGER && pnp_id->type == LAI_INTEGER) {
        if (id.integer == pnp_id->integer) {
            ret = 0;
        }
    } else if (id.type == LAI_STRING && pnp_id->type == LAI_STRING) {
        if (!lai_strcmp(lai_exec_string_access(&id),
                         lai_exec_string_access(pnp_id))) {    
            ret = 0;
        }
    }

    lai_var_finalize(&id);
    return ret;
}

lai_nsnode_t *lai_ns_iterate(struct lai_ns_iterator *iter) {
    while (iter->i < lai_ns_size) {
        lai_nsnode_t *n = lai_namespace[iter->i++];
        if (n)
            return n;
    }

    return NULL;
}

lai_nsnode_t *lai_ns_child_iterate(struct lai_ns_child_iterator *iter) {
    while (iter->i < iter->parent->children.elem_capacity) {
        lai_nsnode_t *n = iter->parent->children.elem_ptr_tab[iter->i++];
        if (n)
            return n;
    }

    return NULL;
}

lai_api_error_t lai_ns_override_opregion(lai_nsnode_t *node, const struct lai_opregion_override *override, void *userptr){
    if(node->type != LAI_NAMESPACE_OPREGION){
        lai_warn("Tried to override opregion functions for non-opregion");
        return LAI_ERROR_TYPE_MISMATCH;
    }

    node->op_override = override;
    node->op_userptr = userptr;
    return LAI_ERROR_NONE;
}