/*
 * everarch - the hopefully ever lasting archive
 * Copyright (C) 2021-2022  Markus Peröbner
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "claims.h"

#include "basics.h"
#include "errors.h"
#include "logger.h"

const char *evr_claim_encoding = "utf-8";
const char *evr_iso_8601_timestamp = "%FT%TZ";
const char *evr_claims_ns = "https://evr.ma300k.de/claims/";
const char *evr_dc_ns = "http://purl.org/dc/terms/";

int evr_init_claim_set(struct evr_claim_set *cs, const evr_time *created){
    cs->out = xmlBufferCreate();
    if(cs->out == NULL){
        goto out;
    }
    cs->writer = xmlNewTextWriterMemory(cs->out, 0);
    if(cs->writer == NULL){
        goto out_with_free_out;
    }
    // claims must be indented because humans can better read it, but
    // also because gpgme's sign operation truncates lines over ~20k
    // characters length.
    if(xmlTextWriterSetIndent(cs->writer, 1) != 0){
        goto out_with_free_writer;
    }
    if(xmlTextWriterStartDocument(cs->writer, NULL, evr_claim_encoding, NULL) < 0){
        goto out_with_free_writer;
    }
    if(xmlTextWriterStartElementNS(cs->writer, NULL, BAD_CAST "claim-set", BAD_CAST evr_claims_ns) < 0){
        goto out_with_free_writer;
    }
    char buf[30];
    evr_time_to_iso8601(buf, sizeof(buf), created);
    if(xmlTextWriterWriteAttributeNS(cs->writer, BAD_CAST "dc", BAD_CAST "created", BAD_CAST evr_dc_ns, BAD_CAST buf) < 0){
        goto out_with_free_writer;
    }
    return evr_ok;
 out_with_free_writer:
    xmlFreeTextWriter(cs->writer);
 out_with_free_out:
    xmlBufferFree(cs->out);
 out:
    return evr_error;
}

int evr_append_file_claim(struct evr_claim_set *cs, const struct evr_file_claim *claim){
    if(xmlTextWriterStartElement(cs->writer, BAD_CAST "file") < 0){
        goto out;
    }
    if(claim->title && claim->title[0] != '\0' && xmlTextWriterWriteAttributeNS(cs->writer, BAD_CAST "dc", BAD_CAST "title", NULL, BAD_CAST claim->title) < 0){
        goto out;
    }
    if(xmlTextWriterStartElement(cs->writer, BAD_CAST "body") < 0){
        goto out;
    }
    const struct evr_file_slice *end = &claim->slices[claim->slices_len];
    evr_blob_ref_str fmt_key;
    char buf[9 + 1];
    for(const struct evr_file_slice *s = claim->slices; s != end; ++s){
        if(xmlTextWriterStartElement(cs->writer, BAD_CAST "slice") < 0){
            goto out;
        }
        evr_fmt_blob_ref(fmt_key, s->ref);
        if(xmlTextWriterWriteAttribute(cs->writer, BAD_CAST "ref", BAD_CAST fmt_key) < 0){
            goto out;
        }
        if(s->size >= 100 << 20){
            goto out;
        }
        sprintf(buf, "%lu", s->size);
        if(xmlTextWriterWriteAttribute(cs->writer, BAD_CAST "size", BAD_CAST buf) < 0){
            goto out;
        }
        // end segment element
        if(xmlTextWriterEndElement(cs->writer) < 0){
            goto out;
        }
    }
    // end body element
    if(xmlTextWriterEndElement(cs->writer) < 0){
        goto out;
    }
    // end file element
    if(xmlTextWriterEndElement(cs->writer) < 0){
        goto out;
    }
    return evr_ok;
 out:
    return evr_error;
}

int evr_finalize_claim_set(struct evr_claim_set *cs){
    int ret = evr_error;
    // end claim-set element
    if(xmlTextWriterEndElement(cs->writer) < 0){
        goto out;
    }
    if(xmlTextWriterEndDocument(cs->writer) < 0){
        goto out;
    }
    xmlFreeTextWriter(cs->writer);
    cs->writer = NULL;
    ret = evr_ok;
 out:
    return ret;
}

int evr_free_claim_set(struct evr_claim_set *cs){
    if(cs->writer != NULL){
        xmlFreeTextWriter(cs->writer);
    }
    xmlBufferFree(cs->out);
    return evr_ok;
}

xmlDocPtr evr_parse_claim_set(const char *buf, size_t buf_size){
    return xmlReadMemory(buf, buf_size, NULL, "UTF-8", 0);
}

xmlNode *evr_get_root_claim_set(xmlDocPtr doc){
    xmlNode *cs = NULL;
    xmlNode *n = xmlDocGetRootElement(doc);
    if(!evr_is_evr_element(n, "claim-set")){
        goto out;
    }
    cs = n;
 out:
    return cs;
}

int evr_parse_created(evr_time *t, xmlNode *node){
    int ret = evr_error;
    char *s = (char*)xmlGetNsProp(node, BAD_CAST "created", BAD_CAST evr_dc_ns);
    if(!s){
        goto out;
    }
    if(evr_time_from_iso8601(t, s) != evr_ok){
        goto out_with_free_s;
    }
    ret = evr_ok;
 out_with_free_s:
    xmlFree(s);
 out:
    return ret;
}

xmlNode *evr_first_claim(xmlNode *claim_set){
    return evr_find_next_element(claim_set->children, NULL);
}

xmlNode *evr_next_claim(xmlNode *claim_node){
    return evr_find_next_element(claim_node->next, NULL);
}

xmlNode *evr_nth_claim(xmlNode *claim_set, int n){
    xmlNode *node = evr_first_claim(claim_set);
    for(int i = 1; node && i < n; ++i){
        node = evr_next_claim(node);
    }
    return node;
}

int evr_is_evr_element(xmlNode *n, char *name){
    int ret = 0;
    if(!n || !n->name || !n->ns || n->type != XML_ELEMENT_NODE){
        goto out;
    }
    if(strcmp((char*)n->name, name) != 0){
        goto out;
    }
    if(strcmp(evr_claims_ns, (char*)n->ns->href) != 0){
        goto out;
    }
    ret = 1;
 out:
    return ret;
}

struct evr_attr_spec_claim *evr_parse_attr_spec_claim(xmlNode *claim_node){
    struct evr_attr_spec_claim *c = NULL;
    size_t attr_def_count = 0;
    size_t attr_def_str_size_sum = 0;
    xmlNode *attr_def_node = claim_node->children;
    while(1){
        attr_def_node = evr_find_next_element(attr_def_node, "attr-def");
        if(!attr_def_node){
            break;
        }
        ++attr_def_count;
        char *key = (char*)xmlGetProp(attr_def_node, BAD_CAST "k");
        if(!key){
            log_error("attr-def is missing k attribute");
            goto out;
        }
        attr_def_str_size_sum += strlen(key) + 1;
        xmlFree(key);
        attr_def_node = attr_def_node->next;
    }
    size_t attr_factories_len = 0;
    xmlNode *attr_factory_node = claim_node->children;
    while(1){
        attr_factory_node = evr_find_next_element(attr_factory_node, "attr-factory");
        if(!attr_factory_node){
            break;
        }
        ++attr_factories_len;
        attr_factory_node = attr_factory_node->next;
    }
    xmlNode *transformation_node = evr_find_next_element(claim_node->children, "transformation");
    if(!transformation_node){
        log_error("Missing transformation element in attr-spec claim");
        goto out;
    }
    char *transformation_type = (char*)xmlGetProp(transformation_node, BAD_CAST "type");
    if(!transformation_type){
        log_error("Missing transformation type on transformation element.");
        goto out;
    }
    if(strcmp(transformation_type, "xslt") != 0){
        log_error("Unsupported transformation type %s found on transformation element", transformation_type);
        xmlFree(transformation_type);
        goto out;
    }
    xmlFree(transformation_type);
    char *fmt_transformation_ref = (char*)xmlGetProp(transformation_node, BAD_CAST "blob");
    evr_blob_ref transformation_ref;
    int parse_transformation_ref_result = evr_parse_blob_ref(transformation_ref, fmt_transformation_ref);
    xmlFree(fmt_transformation_ref);
    if(parse_transformation_ref_result != evr_ok){
        goto out;
    }
    struct evr_buf_pos bp;
    evr_malloc_buf_pos(&bp, sizeof(struct evr_attr_spec_claim) + attr_def_count * sizeof(struct evr_attr_def) + attr_def_str_size_sum + attr_factories_len * evr_blob_ref_size);
    if(!bp.buf){
        goto out;
    }
    c = (struct evr_attr_spec_claim*)bp.pos;
    bp.pos += sizeof(struct evr_attr_spec_claim);
    c->attr_def_len = attr_def_count;
    c->attr_def = (struct evr_attr_def*)bp.pos;
    bp.pos += attr_def_count * sizeof(struct evr_attr_def);
    memcpy(c->transformation_blob_ref, transformation_ref, evr_blob_ref_size);
    struct evr_attr_def *next_attr_def = c->attr_def;
    attr_def_node = claim_node->children;
    while(1){
        attr_def_node = evr_find_next_element(attr_def_node, "attr-def");
        if(!attr_def_node){
            break;
        }
        char *type_name = (char*)xmlGetProp(attr_def_node, BAD_CAST "type");
        if(!type_name){
            log_error("attr-def is missing type attribute");
            goto fail_with_free_c;
        }
        int type;
        if(strcmp(type_name, "str") == 0){
            type = evr_type_str;
        } else if(strcmp(type_name, "int") == 0){
            type = evr_type_int;
        } else {
            log_error("Found unknown type '%s' in attr-def", type_name);
            xmlFree(type_name);
            goto fail_with_free_c;
        }
        xmlFree(type_name);
        char *key = (char*)xmlGetProp(attr_def_node, BAD_CAST "k");
        if(!key){
            goto fail_with_free_c;
        }
        size_t key_size = strlen(key) + 1;
        next_attr_def->key = bp.pos;
        bp.pos += key_size;
        memcpy(next_attr_def->key, key, key_size);
        xmlFree(key);
        next_attr_def->type = type;
        ++next_attr_def;
        attr_def_node = attr_def_node->next;
    }
    evr_blob_ref *attr_factories = (evr_blob_ref*)bp.pos;
    c->attr_factories_len = attr_factories_len;
    c->attr_factories = attr_factories;
    attr_factory_node = claim_node->children;
    while(1){
        attr_factory_node = evr_find_next_element(attr_factory_node, "attr-factory");
        if(!attr_factory_node){
            break;
        }
        char *type_str = (char*)xmlGetProp(attr_factory_node, BAD_CAST "type");
        if(!type_str){
            log_error("Missing type attribute in attr-factory element");
            goto fail_with_free_c;
        }
        if(strcmp(type_str, "executable") != 0){
            log_error("Unknown type attribute found in attr-factory with value: %s", type_str);
            xmlFree(type_str);
            goto fail_with_free_c;
        }
        xmlFree(type_str);
        char *ref_str = (char*)xmlGetProp(attr_factory_node, BAD_CAST "blob");
        if(!ref_str){
            log_error("Missing blob attribute in attr-factory element");
            goto fail_with_free_c;
        }
        if(evr_parse_blob_ref(*attr_factories, ref_str) != evr_ok){
            log_error("Unable to parse blob attribute in attr-factory with value: %s", ref_str);
            xmlFree(ref_str);
            goto fail_with_free_c;
        }
        xmlFree(ref_str);
        ++attr_factories;
        attr_factory_node = attr_factory_node->next;
    }
 out:
    return c;
 fail_with_free_c:
    free(c);
    return NULL;
}

size_t evr_count_elements(xmlNode *start, char *name);

struct evr_file_claim *evr_parse_file_claim(xmlNode *claim_node){
    struct evr_file_claim *c = NULL;
    char *title = (char*)xmlGetNsProp(claim_node, BAD_CAST "title", BAD_CAST evr_dc_ns);
    if(!title){
        // TODO support no title
        goto out;
    }
    xmlNode *body = evr_find_next_element(claim_node->children, "body");
    if(!body){
        goto out_with_free_title;
    }
    size_t slices_count = evr_count_elements(body->children, "slice");
    size_t title_size = strlen(title) + 1;
    char *buf = malloc(sizeof(struct evr_file_claim) + title_size + slices_count * sizeof(struct evr_file_slice));
    if(!buf){
        goto out_with_free_title;
    }
    c = (struct evr_file_claim*)buf;
    buf = (char *)&((struct evr_file_claim*)buf)[1];
    c->title = buf;
    memcpy(c->title, title, title_size);
    buf += title_size;
    c->slices_len = slices_count;
    c->slices = (struct evr_file_slice*)buf;
    int si = 0;
    xmlNode *slice = body->children;
    while(1){
        slice = evr_find_next_element(slice, "slice");
        if(!slice){
            break;
        }
        struct evr_file_slice *s = &c->slices[si];
        char *fmt_ref = (char*)xmlGetProp(slice, BAD_CAST "ref");
        if(!fmt_ref){
            log_error("No ref attribute found on slice element.");
            goto out_with_free_c;
        }
        if(evr_parse_blob_ref(s->ref, fmt_ref) != evr_ok){
            log_error("Illegal ref in claim '%s'", fmt_ref);
            xmlFree(fmt_ref);
            goto out_with_free_c;
        }
        xmlFree(fmt_ref);
        char *fmt_size = (char*)xmlGetProp(slice, BAD_CAST "size");
        if(!fmt_size){
            log_error("No size attribute found on slice element.");
            goto out_with_free_c;
        }
        if(sscanf(fmt_size, "%lu", &s->size) != 1){
            log_error("Illegal size in claim '%s'", fmt_size);
            xmlFree(fmt_size);
            goto out_with_free_c;
        }
        xmlFree(fmt_size);
        ++si;
        slice = slice->next;
    }
 out_with_free_title:
    xmlFree(title);
 out:
    return c;
 out_with_free_c:
    free(c);
    xmlFree(title);
    return NULL;
}

size_t evr_count_elements(xmlNode *start, char *name){
    size_t count = 0;
    while(1){
        start = evr_find_next_element(start, name);
        if(start){
            ++count;
            start = start->next;
        } else {
            break;
        }
    }
    return count;
}

struct evr_attr_claim *evr_parse_attr_claim(xmlNode *claim_node){
    struct evr_attr_claim *c = NULL;
    int ref_type;
    char *fmt_ref = (char*)xmlGetProp(claim_node, BAD_CAST "ref");
    evr_claim_ref ref;
    if(fmt_ref){
        ref_type = evr_ref_type_claim;
        int parse_ref_ret = evr_parse_claim_ref(ref, fmt_ref);
        xmlFree(fmt_ref);
        if(parse_ref_ret != evr_ok){
            goto out;
        }
    } else {
        ref_type = evr_ref_type_self;
    }
    char *fmt_claim_index = (char*)xmlGetProp(claim_node, BAD_CAST "claim");
    size_t claim_index;
    if(fmt_claim_index){
        int scan_res = sscanf(fmt_claim_index, "%lu", &claim_index);
        if(scan_res != 1){
            log_debug("Claim index attribute with value '%s' can't be parsed as decimal number", fmt_claim_index);
            xmlFree(fmt_claim_index);
            goto out;
        }
        xmlFree(fmt_claim_index);
    } else {
        claim_index = 0;
        xmlNode *sibling = claim_node->prev;
        while(sibling){
            if(sibling->type == XML_ELEMENT_NODE){
                ++claim_index;
            }
            sibling = sibling->prev;
        }
    }
    size_t attr_count = 0;
    size_t attr_str_size_sum = 0;
    xmlNode *attr = claim_node->children;
    while(1){
        attr = evr_find_next_element(attr, "a");
        if(!attr){
            break;
        }
        ++attr_count;
        char *key = (char*)xmlGetProp(attr, BAD_CAST "k");
        if(!key){
            log_error("attr claim's a element is missing k attribute");
            goto out;
        }
        attr_str_size_sum += strlen(key) + 1;
        xmlFree(key);
        char *value = (char*)xmlGetProp(attr, BAD_CAST "v");
        if(value){
            attr_str_size_sum += strlen(value) + 1;
            xmlFree(value);
        }
        attr = attr->next;
    }
    char *buf = malloc(sizeof(struct evr_attr_claim) + attr_count * sizeof(struct evr_attr) + attr_str_size_sum);
    if(!buf){
        goto out;
    }
    c = (struct evr_attr_claim *)buf;
    c->ref_type = ref_type;
    buf = (char *)&((struct evr_attr_claim*)buf)[1];
    if(ref_type == evr_ref_type_claim){
        memcpy(c->ref, ref, evr_claim_ref_size);
    }
    c->claim_index = claim_index;
    c->attr_len = attr_count;
    c->attr = (struct evr_attr *)buf;
    buf = (char *)&((struct evr_attr*)buf)[attr_count];
    struct evr_attr *next_attr = c->attr;
    attr = claim_node->children;
    while(1){
        attr = evr_find_next_element(attr, "a");
        if(!attr){
            break;
        }
        char *op_str = (char*)xmlGetProp(attr, BAD_CAST "op");
        if(!op_str){
            log_error("Operator is missing on attr");
            goto fail_with_free_c;
        }
        int op;
        if(strcmp(op_str, "=") == 0){
            op = evr_attr_op_replace;
        } else if(strcmp(op_str, "+") == 0){
            op = evr_attr_op_add;
        } else if(strcmp(op_str, "-") == 0){
            op = evr_attr_op_rm;
        } else {
            log_error("Unknown attr operator '%s'", op_str);
            xmlFree(op_str);
            goto fail_with_free_c;
        }
        xmlFree(op_str);
        char *key = (char*)xmlGetProp(attr, BAD_CAST "k");
        if(!key){
            // no logging here because the size calculation above
            // already checks the presence of k
            goto fail_with_free_c;
        }
        char *value = (char*)xmlGetProp(attr, BAD_CAST "v");
        next_attr->op = op;
        size_t key_size = strlen(key) + 1;
        next_attr->attr.key = buf;
        memcpy(next_attr->attr.key, key, key_size);
        buf += key_size;
        if(value){
            next_attr->attr.value = buf;
            size_t value_size = strlen(value) + 1;
            memcpy(next_attr->attr.value, value, value_size);
            buf += value_size;
        } else {
            next_attr->attr.value = NULL;
        }
        xmlFree(value);
        xmlFree(key);
        ++next_attr;
        attr = attr->next;
    }
 out:
    return c;
 fail_with_free_c:
    free(c);
    return NULL;
}

xmlNode *evr_find_next_element(xmlNode *n, char *name_filter){
    for(xmlNode *c = n; c; c = c->next){
        if(c->type != XML_ELEMENT_NODE){
            continue;
        }
        if(!name_filter || evr_is_evr_element(c, name_filter)){
            return c;
        }
    }
    return NULL;
    
}
