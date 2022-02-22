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

#include "errors.h"
#include "logger.h"

const char *evr_claim_encoding = "utf-8";
const char *evr_iso_8601_timestamp = "%FT%TZ";
const char *evr_claims_ns = "https://evr.ma300k.de/claims/";
const char *evr_dc_ns = "http://purl.org/dc/terms/";

int evr_init_claim_set(struct evr_claim_set *cs, const time_t *created){
    cs->out = xmlBufferCreate();
    if(cs->out == NULL){
        goto out;
    }
    cs->writer = xmlNewTextWriterMemory(cs->out, 0);
    if(cs->writer == NULL){
        goto out_with_free_out;
    }
    if(xmlTextWriterStartDocument(cs->writer, NULL, evr_claim_encoding, NULL) < 0){
        goto out_with_free_writer;
    }
    if(xmlTextWriterStartElementNS(cs->writer, NULL, BAD_CAST "claim-set", BAD_CAST evr_claims_ns) < 0){
        goto out_with_free_writer;
    }
    char buf[30];
    struct tm t;
    gmtime_r(created, &t);
    strftime(buf, sizeof(buf), evr_iso_8601_timestamp, &t);
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
    evr_fmt_blob_key_t fmt_key;
    char buf[9 + 1];
    for(const struct evr_file_slice *s = claim->slices; s != end; ++s){
        if(xmlTextWriterStartElement(cs->writer, BAD_CAST "slice") < 0){
            goto out;
        }
        evr_fmt_blob_key(fmt_key, s->ref);
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

int evr_parse_created(time_t *t, xmlNode *node){
    int ret = evr_error;
    char *s = (char*)xmlGetNsProp(node, BAD_CAST "created", BAD_CAST evr_dc_ns);
    if(!s){
        goto out;
    }
    char *end = &s[strlen(s)];
    struct tm tm;
    if(strptime(s, evr_iso_8601_timestamp, &tm) != end){
        goto out_with_free_s;
    }
    *t = timegm(&tm);
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
    size_t slices_count = 0;
    xmlNode *slice = body->children;
    while(1){
        slice = evr_find_next_element(slice, "slice");
        if(slice){
            ++slices_count;
            slice = slice->next;
        } else {
            break;
        }
    }
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
    slice = body->children;
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
        if(evr_parse_blob_key(s->ref, fmt_ref) != evr_ok){
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
