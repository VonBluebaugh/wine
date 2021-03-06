/*
 * Web Services on Devices
 *
 * Copyright 2017-2018 Owen Rudge for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <limits.h>

#define COBJMACROS

#include "wsdapi_internal.h"
#include "wine/debug.h"
#include "wine/heap.h"
#include "webservices.h"

WINE_DEFAULT_DEBUG_CHANNEL(wsdapi);

#define APP_MAX_DELAY                       500

static const WCHAR discoveryTo[] = {
    'u','r','n',':',
    's','c','h','e','m','a','s','-','x','m','l','s','o','a','p','-','o','r','g',':',
    'w','s',':','2','0','0','5',':','0','4',':',
    'd','i','s','c','o','v','e','r','y', 0 };

static const WCHAR actionHello[] = {
    'h','t','t','p',':','/','/',
    's','c','h','e','m','a','s','.','x','m','l','s','o','a','p','.','o','r','g','/',
    'w','s','/','2','0','0','5','/','0','4','/',
    'd','i','s','c','o','v','e','r','y','/',
    'H','e','l','l','o', 0 };

static const WCHAR addressingNsUri[] = {
    'h','t','t','p',':','/','/',
    's','c','h','e','m','a','s','.','x','m','l','s','o','a','p','.','o','r','g','/',
    'w','s','/','2','0','0','4','/','0','8','/','a','d','d','r','e','s','s','i','n','g', 0 };

static const WCHAR discoveryNsUri[] = {
    'h','t','t','p',':','/','/',
    's','c','h','e','m','a','s','.','x','m','l','s','o','a','p','.','o','r','g','/',
    'w','s','/','2','0','0','5','/','0','4','/','d','i','s','c','o','v','e','r','y', 0 };

static const WCHAR envelopeNsUri[] = {
    'h','t','t','p',':','/','/',
    'w','w','w','.','w','3','.','o','r','g','/',
    '2','0','0','3','/','0','5','/','s','o','a','p','-','e','n','v','e','l','o','p','e', 0 };

static const WCHAR addressingPrefix[] = { 'w','s','a', 0 };
static const WCHAR discoveryPrefix[] = { 'w','s','d', 0 };
static const WCHAR envelopePrefix[] = { 's','o','a','p', 0 };
static const WCHAR headerString[] = { 'H','e','a','d','e','r', 0 };
static const WCHAR actionString[] = { 'A','c','t','i','o','n', 0 };
static const WCHAR messageIdString[] = { 'M','e','s','s','a','g','e','I','D', 0 };
static const WCHAR toString[] = { 'T','o', 0 };
static const WCHAR relatesToString[] = { 'R','e','l','a','t','e','s','T','o', 0 };
static const WCHAR appSequenceString[] = { 'A','p','p','S','e','q','u','e','n','c','e', 0 };
static const WCHAR emptyString[] = { 0 };

struct discovered_namespace
{
    struct list entry;
    LPCWSTR prefix;
    LPCWSTR uri;
};

static char *wide_to_utf8(LPCWSTR wide_string, int *length)
{
    char *new_string = NULL;

    if (wide_string == NULL)
        return NULL;

    *length = WideCharToMultiByte(CP_UTF8, 0, wide_string, -1, NULL, 0, NULL, NULL);

    if (*length < 0)
        return NULL;

    new_string = heap_alloc(*length);
    WideCharToMultiByte(CP_UTF8, 0, wide_string, -1, new_string, *length, NULL, NULL);

    return new_string;
}

static WS_XML_STRING *populate_xml_string(LPCWSTR str)
{
    WS_XML_STRING *xml = heap_alloc_zero(sizeof(WS_XML_STRING));
    int utf8Length;

    if (xml == NULL)
        return NULL;

    xml->bytes = (BYTE *)wide_to_utf8(str, &utf8Length);

    if (xml->bytes == NULL)
    {
        heap_free(xml);
        return NULL;
    }

    xml->dictionary = NULL;
    xml->id = 0;
    xml->length = (xml->bytes == NULL) ? 0 : (utf8Length - 1);

    return xml;
}

static inline void free_xml_string(WS_XML_STRING *value)
{
    if (value == NULL)
        return;

    if (value->bytes != NULL)
        heap_free(value->bytes);

    heap_free(value);
}

static BOOL write_xml_element(WSDXML_ELEMENT *element, WS_XML_WRITER *writer)
{
    WS_XML_STRING *local_name = NULL, *element_ns = NULL, *ns_prefix = NULL;
    WS_XML_UTF16_TEXT utf16_text;
    WSDXML_NODE *current_child;
    WSDXML_TEXT *node_as_text;
    BOOL retVal = FALSE;
    int text_len;
    HRESULT ret;

    if (element == NULL)
        return TRUE;

    /* Start the element */
    local_name = populate_xml_string(element->Name->LocalName);
    if (local_name == NULL) goto cleanup;

    element_ns = populate_xml_string(element->Name->Space->Uri);
    if (element_ns == NULL) goto cleanup;

    ns_prefix = populate_xml_string(element->Name->Space->PreferredPrefix);
    if (ns_prefix == NULL) goto cleanup;

    ret = WsWriteStartElement(writer, ns_prefix, local_name, element_ns, NULL);
    if (FAILED(ret)) goto cleanup;

    /* TODO: Write attributes */

    /* Write child elements */
    current_child = element->FirstChild;

    while (current_child != NULL)
    {
        if (current_child->Type == ElementType)
        {
            if (!write_xml_element((WSDXML_ELEMENT *)current_child, writer)) goto cleanup;
        }
        else if (current_child->Type == TextType)
        {
            node_as_text = (WSDXML_TEXT *)current_child;
            text_len = lstrlenW(node_as_text->Text);

            utf16_text.text.textType = WS_XML_TEXT_TYPE_UTF16;
            utf16_text.byteCount = min(WSD_MAX_TEXT_LENGTH, text_len) * sizeof(WCHAR);
            utf16_text.bytes = (BYTE *)node_as_text->Text;

            ret = WsWriteText(writer, (WS_XML_TEXT *)&utf16_text, NULL);
            if (FAILED(ret)) goto cleanup;
        }

        current_child = current_child->Next;
    }

    /* End the element */
    ret = WsWriteEndElement(writer, NULL);
    if (FAILED(ret)) goto cleanup;

    retVal = TRUE;

cleanup:
    free_xml_string(local_name);
    free_xml_string(element_ns);
    free_xml_string(ns_prefix);

    return retVal;
}

static WSDXML_ELEMENT *add_child_element(IWSDXMLContext *xml_context, WSDXML_ELEMENT *parent, LPCWSTR ns_uri,
    LPCWSTR name, LPCWSTR text)
{
    WSDXML_ELEMENT *element_obj;
    WSDXML_NAME *name_obj;

    if (FAILED(IWSDXMLContext_AddNameToNamespace(xml_context, ns_uri, name, &name_obj)))
        return NULL;

    if (FAILED(WSDXMLBuildAnyForSingleElement(name_obj, text, &element_obj)))
    {
        WSDFreeLinkedMemory(name_obj);
        return NULL;
    }

    WSDFreeLinkedMemory(name_obj);

    /* Add the element as a child - this will link the element's memory allocation to the parent's */
    if (FAILED(WSDXMLAddChild(parent, element_obj)))
    {
        WSDFreeLinkedMemory(element_obj);
        return NULL;
    }

    return element_obj;
}

static BOOL create_guid(LPWSTR buffer)
{
    const WCHAR formatString[] = { 'u','r','n',':','u','u','i','d',':','%','s', 0 };

    WCHAR* uuidString = NULL;
    UUID uuid;

    if (UuidCreate(&uuid) != RPC_S_OK)
        return FALSE;

    UuidToStringW(&uuid, (RPC_WSTR*)&uuidString);

    if (uuidString == NULL)
        return FALSE;

    wsprintfW(buffer, formatString, uuidString);
    RpcStringFreeW((RPC_WSTR*)&uuidString);

    return TRUE;
}

static void populate_soap_header(WSD_SOAP_HEADER *header, LPCWSTR to, LPCWSTR action, LPCWSTR message_id,
    WSD_APP_SEQUENCE *sequence, const WSDXML_ELEMENT *any_headers)
{
    ZeroMemory(header, sizeof(WSD_SOAP_HEADER));

    header->To = to;
    header->Action = action;
    header->MessageID = message_id;
    header->AppSequence = sequence;
    header->AnyHeaders = (WSDXML_ELEMENT *)any_headers;

    /* TODO: Implement RelatesTo, ReplyTo, From, FaultTo */
}

static BOOL add_discovered_namespace(struct list *namespaces, WSDXML_NAMESPACE *discovered_ns)
{
    struct discovered_namespace *ns;

    LIST_FOR_EACH_ENTRY(ns, namespaces, struct discovered_namespace, entry)
    {
        if (lstrcmpW(ns->uri, discovered_ns->Uri) == 0)
            return TRUE; /* Already added */
    }

    ns = WSDAllocateLinkedMemory(namespaces, sizeof(struct discovered_namespace));

    if (ns == NULL)
        return FALSE;

    ns->prefix = duplicate_string(ns, discovered_ns->PreferredPrefix);
    ns->uri = duplicate_string(ns, discovered_ns->Uri);

    if ((ns->prefix == NULL) || (ns->uri == NULL))
        return FALSE;

    list_add_tail(namespaces, &ns->entry);
    return TRUE;
}

static WSDXML_ELEMENT *create_soap_header_xml_elements(IWSDXMLContext *xml_context, WSD_SOAP_HEADER *header)
{
    WSDXML_ELEMENT *header_element = NULL, *app_sequence_element = NULL;
    WSDXML_NAME *header_name = NULL;

    /* <s:Header> */
    if (FAILED(IWSDXMLContext_AddNameToNamespace(xml_context, envelopeNsUri, headerString, &header_name))) goto cleanup;
    if (FAILED(WSDXMLBuildAnyForSingleElement(header_name, NULL, &header_element))) goto cleanup;
    WSDFreeLinkedMemory(header_name);

    /* <a:Action> */
    if (add_child_element(xml_context, header_element, addressingNsUri, actionString, header->Action) == NULL)
        goto cleanup;

    /* <a:MessageId> */
    if (add_child_element(xml_context, header_element, addressingNsUri, messageIdString, header->MessageID) == NULL)
        goto cleanup;

    /* <a:To> */
    if (add_child_element(xml_context, header_element, addressingNsUri, toString, header->To) == NULL)
        goto cleanup;

    /* <a:RelatesTo> */
    if (header->RelatesTo.MessageID != NULL)
    {
        if (add_child_element(xml_context, header_element, addressingNsUri, relatesToString,
            header->RelatesTo.MessageID) == NULL) goto cleanup;
    }

    /* <d:AppSequence> */
    app_sequence_element = add_child_element(xml_context, header_element, discoveryNsUri, appSequenceString, emptyString);
    if (app_sequence_element == NULL) goto cleanup;

    /* TODO: InstanceId attribute */

    /* TODO: MessageNumber attribute */

    /* TODO: SequenceID attribute */

    /* </d:AppSequence> */

    /* TODO: Write any headers */

    /* </s:Header> */

    return header_element;

cleanup:
    if (header_name != NULL) WSDFreeLinkedMemory(header_name);
    WSDXMLCleanupElement(header_element);

    return NULL;
}

static HRESULT create_soap_envelope(IWSDXMLContext *xml_context, WSD_SOAP_HEADER *header, WSDXML_ELEMENT *body_element,
    WS_HEAP **heap, char **output_xml, ULONG *xml_length, struct list *discovered_namespaces)
{
    WS_XML_STRING *actual_envelope_prefix = NULL, *envelope_uri_xmlstr = NULL, *tmp_prefix = NULL, *tmp_uri = NULL;
    WSDXML_NAMESPACE *addressing_ns = NULL, *discovery_ns = NULL, *envelope_ns = NULL;
    WSDXML_ELEMENT *header_element = NULL;
    struct discovered_namespace *ns;
    WS_XML_BUFFER *buffer = NULL;
    WS_XML_WRITER *writer = NULL;
    WS_XML_STRING envelope;
    HRESULT ret = E_OUTOFMEMORY;
    static BYTE envelopeString[] = "Envelope";

    /* Create the necessary XML prefixes */
    if (FAILED(IWSDXMLContext_AddNamespace(xml_context, addressingNsUri, addressingPrefix, &addressing_ns))) goto cleanup;
    if (!add_discovered_namespace(discovered_namespaces, addressing_ns)) goto cleanup;

    if (FAILED(IWSDXMLContext_AddNamespace(xml_context, discoveryNsUri, discoveryPrefix, &discovery_ns))) goto cleanup;
    if (!add_discovered_namespace(discovered_namespaces, discovery_ns)) goto cleanup;

    if (FAILED(IWSDXMLContext_AddNamespace(xml_context, envelopeNsUri, envelopePrefix, &envelope_ns))) goto cleanup;
    if (!add_discovered_namespace(discovered_namespaces, envelope_ns)) goto cleanup;

    envelope.bytes = envelopeString;
    envelope.length = sizeof(envelopeString) - 1;
    envelope.dictionary = NULL;
    envelope.id = 0;

    actual_envelope_prefix = populate_xml_string(envelope_ns->PreferredPrefix);
    envelope_uri_xmlstr = populate_xml_string(envelope_ns->Uri);

    if ((actual_envelope_prefix == NULL) || (envelope_uri_xmlstr == NULL)) goto cleanup;

    /* Now try to create the appropriate WebServices buffers, etc */
    ret = WsCreateHeap(16384, 4096, NULL, 0, heap, NULL);
    if (FAILED(ret)) goto cleanup;

    ret = WsCreateXmlBuffer(*heap, NULL, 0, &buffer, NULL);
    if (FAILED(ret)) goto cleanup;

    ret = WsCreateWriter(NULL, 0, &writer, NULL);
    if (FAILED(ret)) goto cleanup;

    ret = WsSetOutputToBuffer(writer, buffer, NULL, 0, NULL);
    if (FAILED(ret)) goto cleanup;

    /* Create the header XML elements */
    header_element = create_soap_header_xml_elements(xml_context, header);
    if (header_element == NULL) goto cleanup;

    /* <s:Envelope> */
    ret = WsWriteStartElement(writer, actual_envelope_prefix, &envelope, envelope_uri_xmlstr, NULL);
    if (FAILED(ret)) goto cleanup;

    LIST_FOR_EACH_ENTRY(ns, discovered_namespaces, struct discovered_namespace, entry)
    {
        tmp_prefix = populate_xml_string(ns->prefix);
        tmp_uri = populate_xml_string(ns->uri);

        if ((tmp_prefix == NULL) || (tmp_uri == NULL)) goto cleanup;

        ret = WsWriteXmlnsAttribute(writer, tmp_prefix, tmp_uri, FALSE, NULL);
        if (FAILED(ret)) goto cleanup;

        free_xml_string(tmp_prefix);
        free_xml_string(tmp_uri);
    }

    tmp_prefix = NULL;
    tmp_uri = NULL;

    /* Write the header */
    if (!write_xml_element(header_element, writer)) goto cleanup;

    ret = WsWriteEndElement(writer, NULL);
    if (FAILED(ret)) goto cleanup;

    /* </s:Envelope> */

    /* Generate the bytes of the document */
    ret = WsWriteXmlBufferToBytes(writer, buffer, NULL, NULL, 0, *heap, (void**)output_xml, xml_length, NULL);
    if (FAILED(ret)) goto cleanup;

cleanup:
    WSDFreeLinkedMemory(addressing_ns);
    WSDFreeLinkedMemory(discovery_ns);
    WSDFreeLinkedMemory(envelope_ns);

    WSDXMLCleanupElement(header_element);

    free_xml_string(actual_envelope_prefix);
    free_xml_string(envelope_uri_xmlstr);

    if (writer != NULL)
        WsFreeWriter(writer);

    /* Don't free the heap unless the operation has failed */
    if ((FAILED(ret)) && (*heap != NULL))
    {
        WsFreeHeap(*heap);
        *heap = NULL;
    }

    return ret;
}

static HRESULT write_and_send_message(IWSDiscoveryPublisherImpl *impl, WSD_SOAP_HEADER *header, WSDXML_ELEMENT *body_element,
    struct list *discovered_namespaces, IWSDUdpAddress *remote_address, int max_initial_delay)
{
    static const char xml_header[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
    ULONG xml_length = 0, xml_header_len = sizeof(xml_header) - 1;
    WS_HEAP *heap = NULL;
    char *xml = NULL;
    char *full_xml;
    HRESULT ret;

    ret = create_soap_envelope(impl->xmlContext, header, NULL, &heap, &xml, &xml_length, discovered_namespaces);
    if (ret != S_OK) return ret;

    /* Prefix the XML header */
    full_xml = heap_alloc(xml_length + xml_header_len + 1);

    if (full_xml == NULL)
    {
        WsFreeHeap(heap);
        return E_OUTOFMEMORY;
    }

    memcpy(full_xml, xml_header, xml_header_len);
    memcpy(full_xml + xml_header_len, xml, xml_length);
    full_xml[xml_length + xml_header_len] = 0;

    if (remote_address == NULL)
    {
        /* Send the message via UDP multicast */
        ret = send_udp_multicast(impl, full_xml, xml_length + xml_header_len + 1, max_initial_delay) ? S_OK : E_FAIL;
    }
    else
    {
        /* TODO: Send the message via UDP unicast */
        FIXME("TODO: Send the message via UDP unicast\n");
    }

    heap_free(full_xml);
    WsFreeHeap(heap);

    return ret;
}

HRESULT send_hello_message(IWSDiscoveryPublisherImpl *impl, LPCWSTR id, ULONGLONG metadata_ver, ULONGLONG instance_id,
    ULONGLONG msg_num, LPCWSTR session_id, const WSD_NAME_LIST *types_list, const WSD_URI_LIST *scopes_list,
    const WSD_URI_LIST *xaddrs_list, const WSDXML_ELEMENT *hdr_any, const WSDXML_ELEMENT *ref_param_any,
    const WSDXML_ELEMENT *endpoint_ref_any, const WSDXML_ELEMENT *any)
{
    struct list *discoveredNamespaces = NULL;
    WSD_SOAP_HEADER soapHeader;
    WSD_APP_SEQUENCE sequence;
    WCHAR message_id[64];
    HRESULT ret = E_OUTOFMEMORY;

    sequence.InstanceId = instance_id;
    sequence.MessageNumber = msg_num;
    sequence.SequenceId = session_id;

    if (!create_guid(message_id)) goto cleanup;

    discoveredNamespaces = WSDAllocateLinkedMemory(NULL, sizeof(struct list));
    if (!discoveredNamespaces) goto cleanup;

    list_init(discoveredNamespaces);

    populate_soap_header(&soapHeader, discoveryTo, actionHello, message_id, &sequence, hdr_any);

    /* TODO: Populate message body */

    /* Write and send the message */
    ret = write_and_send_message(impl, &soapHeader, NULL, discoveredNamespaces, NULL, APP_MAX_DELAY);

cleanup:
    WSDFreeLinkedMemory(discoveredNamespaces);

    return ret;
}
