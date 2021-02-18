/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2012 Google, Inc.
 */

#include <stdio.h>
#include <stdlib.h>

#include <libqmi-glib.h>

#include <ModemManager.h>
#include <mm-errors-types.h>

#include "mm-port-qmi.h"
#include "mm-modem-helpers-qmi.h"
#include "mm-log-object.h"

G_DEFINE_TYPE (MMPortQmi, mm_port_qmi, MM_TYPE_PORT)

typedef struct {
    QmiService     service;
    QmiClient     *client;
    MMPortQmiFlag  flag;
} ServiceInfo;

struct _MMPortQmiPrivate {
    gboolean   in_progress;
    QmiDevice *qmi_device;
    GList     *services;
    /* endpoint info */
    gulong              endpoint_info_signal_id;
    QmiDataEndpointType endpoint_type;
    gint                endpoint_interface_number;
    /* kernel data format */
    QmiDeviceExpectedDataFormat kernel_data_format;
    /* wda settings */
    gboolean                wda_unsupported;
    QmiWdaLinkLayerProtocol llp;
};

/*****************************************************************************/

static QmiClient *
lookup_client (MMPortQmi     *self,
               QmiService     service,
               MMPortQmiFlag  flag,
               gboolean       steal)
{
    GList *l;

    for (l = self->priv->services; l; l = g_list_next (l)) {
        ServiceInfo *info = l->data;

        if (info->service == service && info->flag == flag) {
            QmiClient *found;

            found = info->client;
            if (steal) {
                self->priv->services = g_list_delete_link (self->priv->services, l);
                g_free (info);
            }
            return found;
        }
    }

    return NULL;
}

QmiClient *
mm_port_qmi_peek_client (MMPortQmi *self,
                         QmiService service,
                         MMPortQmiFlag flag)
{
    return lookup_client (self, service, flag, FALSE);
}

QmiClient *
mm_port_qmi_get_client (MMPortQmi *self,
                        QmiService service,
                        MMPortQmiFlag flag)
{
    QmiClient *client;

    client = mm_port_qmi_peek_client (self, service, flag);
    return (client ? g_object_ref (client) : NULL);
}

/*****************************************************************************/

QmiDevice *
mm_port_qmi_peek_device (MMPortQmi *self)
{
    g_return_val_if_fail (MM_IS_PORT_QMI (self), NULL);

    return self->priv->qmi_device;
}

/*****************************************************************************/

static void
initialize_endpoint_info (MMPortQmi *self)
{
    MMKernelDevice *kernel_device;

    kernel_device = mm_port_peek_kernel_device (MM_PORT (self));

    if (!kernel_device)
        self->priv->endpoint_type = QMI_DATA_ENDPOINT_TYPE_UNDEFINED;
    else
        self->priv->endpoint_type = mm_port_subsys_to_qmi_endpoint_type (mm_port_get_subsys (MM_PORT (self)));

    switch (self->priv->endpoint_type) {
        case QMI_DATA_ENDPOINT_TYPE_HSUSB:
            g_assert (kernel_device);
            self->priv->endpoint_interface_number = mm_kernel_device_get_interface_number (kernel_device);
            break;
        case QMI_DATA_ENDPOINT_TYPE_EMBEDDED:
            self->priv->endpoint_interface_number = 1;
            break;
        case QMI_DATA_ENDPOINT_TYPE_PCIE:
        case QMI_DATA_ENDPOINT_TYPE_UNDEFINED:
        case QMI_DATA_ENDPOINT_TYPE_HSIC:
        case QMI_DATA_ENDPOINT_TYPE_BAM_DMUX:
        case QMI_DATA_ENDPOINT_TYPE_UNKNOWN:
        default:
            self->priv->endpoint_interface_number = 0;
            break;
    }

    mm_obj_dbg (self, "endpoint info updated: type '%s', interface number '%u'",
                qmi_data_endpoint_type_get_string (self->priv->endpoint_type),
                self->priv->endpoint_interface_number);
}

QmiDataEndpointType
mm_port_qmi_get_endpoint_type (MMPortQmi *self)
{
    return self->priv->endpoint_type;
}

guint
mm_port_qmi_get_endpoint_interface_number (MMPortQmi *self)
{
    return self->priv->endpoint_interface_number;
}

/*****************************************************************************/

void
mm_port_qmi_release_client (MMPortQmi     *self,
                            QmiService     service,
                            MMPortQmiFlag  flag)
{
    QmiClient *client;

    if (!self->priv->qmi_device)
        return;

    client = lookup_client (self, service, flag, TRUE);
    if (!client)
        return;

    mm_obj_dbg (self, "explicitly releasing client for service '%s'...", qmi_service_get_string (service));
    qmi_device_release_client (self->priv->qmi_device,
                               client,
                               QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
                               3, NULL, NULL, NULL);
    g_object_unref (client);
}

/*****************************************************************************/

typedef struct {
    ServiceInfo *info;
} AllocateClientContext;

static void
allocate_client_context_free (AllocateClientContext *ctx)
{
    if (ctx->info) {
        g_assert (ctx->info->client == NULL);
        g_free (ctx->info);
    }
    g_free (ctx);
}

gboolean
mm_port_qmi_allocate_client_finish (MMPortQmi *self,
                                    GAsyncResult *res,
                                    GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
allocate_client_ready (QmiDevice *qmi_device,
                       GAsyncResult *res,
                       GTask *task)
{
    MMPortQmi *self;
    AllocateClientContext *ctx;
    GError *error = NULL;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);
    ctx->info->client = qmi_device_allocate_client_finish (qmi_device, res, &error);
    if (!ctx->info->client) {
        g_prefix_error (&error,
                        "Couldn't create client for service '%s': ",
                        qmi_service_get_string (ctx->info->service));
        g_task_return_error (task, error);
    } else {
        /* Move the service info to our internal list */
        self->priv->services = g_list_prepend (self->priv->services, ctx->info);
        ctx->info = NULL;
        g_task_return_boolean (task, TRUE);
    }

    g_object_unref (task);
}

void
mm_port_qmi_allocate_client (MMPortQmi *self,
                             QmiService service,
                             MMPortQmiFlag flag,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
    AllocateClientContext *ctx;
    GTask *task;

    task = g_task_new (self, cancellable, callback, user_data);

    if (!mm_port_qmi_is_open (self)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
                                 "Port is closed");
        g_object_unref (task);
        return;
    }

    if (!!mm_port_qmi_peek_client (self, service, flag)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_EXISTS,
                                 "Client for service '%s' already allocated",
                                 qmi_service_get_string (service));
        g_object_unref (task);
        return;
    }

    ctx = g_new0 (AllocateClientContext, 1);
    ctx->info = g_new0 (ServiceInfo, 1);
    ctx->info->service = service;
    ctx->info->flag = flag;
    g_task_set_task_data (task, ctx, (GDestroyNotify)allocate_client_context_free);

    qmi_device_allocate_client (self->priv->qmi_device,
                                service,
                                QMI_CID_NONE,
                                10,
                                cancellable,
                                (GAsyncReadyCallback)allocate_client_ready,
                                task);
}

/*****************************************************************************/

QmiWdaLinkLayerProtocol
mm_port_qmi_get_link_layer_protocol (MMPortQmi *self)
{
    return self->priv->llp;
}

/*****************************************************************************/

static QmiDeviceExpectedDataFormat
load_kernel_data_format_current (MMPortQmi *self,
                                 QmiDevice *device)
{
    QmiDeviceExpectedDataFormat value;

    /* For any driver other than qmi_wwan, assume raw-ip */
    if (mm_port_get_subsys (MM_PORT (self)) != MM_PORT_SUBSYS_USBMISC)
        return QMI_DEVICE_EXPECTED_DATA_FORMAT_RAW_IP;

    /* If the expected data format is unknown, it means the kernel in use
     * doesn't have support for querying it; therefore it's 802.3 */
    value = qmi_device_get_expected_data_format (device, NULL);
    if (value == QMI_DEVICE_EXPECTED_DATA_FORMAT_UNKNOWN)
        value = QMI_DEVICE_EXPECTED_DATA_FORMAT_802_3;

    return value;
}

static void
load_kernel_data_format_capabilities (MMPortQmi *self,
                                      QmiDevice *device,
                                      gboolean  *supports_802_3,
                                      gboolean  *supports_raw_ip)
{
    /* For any driver other than qmi_wwan, assume raw-ip */
    if (mm_port_get_subsys (MM_PORT (self)) != MM_PORT_SUBSYS_USBMISC) {
        *supports_802_3 = FALSE;
        *supports_raw_ip = TRUE;
        return;
    }

    *supports_802_3 = TRUE;
    *supports_raw_ip = qmi_device_check_expected_data_format_supported (device,
                                                                        QMI_DEVICE_EXPECTED_DATA_FORMAT_RAW_IP,
                                                                        NULL);}

/*****************************************************************************/

typedef struct {
    QmiDeviceExpectedDataFormat kernel_data_format;
    QmiWdaLinkLayerProtocol     wda_llp;
} DataFormatCombination;

static const DataFormatCombination data_format_combinations[] = {
    { QMI_DEVICE_EXPECTED_DATA_FORMAT_RAW_IP, QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP },
    { QMI_DEVICE_EXPECTED_DATA_FORMAT_802_3,  QMI_WDA_LINK_LAYER_PROTOCOL_802_3  },
};

typedef enum {
    INTERNAL_SETUP_DATA_FORMAT_STEP_FIRST,
    INTERNAL_SETUP_DATA_FORMAT_STEP_KERNEL_DATA_FORMAT_CAPABILITIES,
    INTERNAL_SETUP_DATA_FORMAT_STEP_RETRY,
    INTERNAL_SETUP_DATA_FORMAT_STEP_KERNEL_DATA_FORMAT_CURRENT,
    INTERNAL_SETUP_DATA_FORMAT_STEP_ALLOCATE_WDA_CLIENT,
    INTERNAL_SETUP_DATA_FORMAT_STEP_GET_WDA_DATA_FORMAT,
    INTERNAL_SETUP_DATA_FORMAT_STEP_QUERY_DONE,
    INTERNAL_SETUP_DATA_FORMAT_STEP_CHECK_DATA_FORMAT,
    INTERNAL_SETUP_DATA_FORMAT_STEP_SYNC_WDA_DATA_FORMAT,
    INTERNAL_SETUP_DATA_FORMAT_STEP_SYNC_KERNEL_DATA_FORMAT,
    INTERNAL_SETUP_DATA_FORMAT_STEP_LAST,
} InternalSetupDataFormatStep;

typedef struct {
    QmiDevice                      *device;
    MMPortQmiSetupDataFormatAction  action;

    InternalSetupDataFormatStep step;
    gboolean                    use_endpoint;
    gint                        data_format_combination_i;

    /* configured kernel data format, mainly when using qmi_wwan */
    QmiDeviceExpectedDataFormat kernel_data_format_current;
    QmiDeviceExpectedDataFormat kernel_data_format_requested;
    gboolean                    kernel_data_format_802_3_supported;
    gboolean                    kernel_data_format_raw_ip_supported;

    /* configured device data format */
    QmiClient               *wda;
    QmiWdaLinkLayerProtocol  wda_llp_current;
    QmiWdaLinkLayerProtocol  wda_llp_requested;
} InternalSetupDataFormatContext;

static void
internal_setup_data_format_context_free (InternalSetupDataFormatContext *ctx)
{
    if (ctx->wda && ctx->device)
        qmi_device_release_client (ctx->device,
                                   ctx->wda,
                                   QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
                                   3, NULL, NULL, NULL);
    g_clear_object (&ctx->wda);
    g_clear_object (&ctx->device);
    g_slice_free (InternalSetupDataFormatContext, ctx);
}

static gboolean
internal_setup_data_format_finish (MMPortQmi                    *self,
                                   GAsyncResult                 *res,
                                   QmiDeviceExpectedDataFormat  *out_kernel_data_format,
                                   QmiWdaLinkLayerProtocol      *out_llp,
                                   GError                      **error)
{
    InternalSetupDataFormatContext *ctx;

    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;

    ctx = g_task_get_task_data (G_TASK (res));
    *out_kernel_data_format = ctx->kernel_data_format_current;
    *out_llp = ctx->wda_llp_current;
    return TRUE;
}

static void internal_setup_data_format_context_step (GTask *task);

static void
sync_kernel_data_format (GTask *task)
{
    MMPortQmi              *self;
    InternalSetupDataFormatContext *ctx;
    GError                 *error = NULL;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    mm_obj_dbg (self, "Updating kernel expected data format: %s -> %s",
                qmi_device_expected_data_format_get_string (ctx->kernel_data_format_current),
                qmi_device_expected_data_format_get_string (ctx->kernel_data_format_requested));

    if (!qmi_device_set_expected_data_format (ctx->device,
                                              ctx->kernel_data_format_requested,
                                              &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* request reload */
    ctx->kernel_data_format_current = QMI_DEVICE_EXPECTED_DATA_FORMAT_UNKNOWN;

    /* Go on to next step */
    ctx->step++;
    internal_setup_data_format_context_step (task);
}

static void
set_data_format_ready (QmiClientWda *client,
                       GAsyncResult *res,
                       GTask        *task)
{
    InternalSetupDataFormatContext              *ctx;
    g_autoptr(QmiMessageWdaSetDataFormatOutput)  output = NULL;
    g_autoptr(GError)                            error = NULL;

    ctx = g_task_get_task_data (task);

    output = qmi_client_wda_set_data_format_finish (client, res, &error);
    if (!output || !qmi_message_wda_set_data_format_output_get_result (output, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* request reload */
    ctx->wda_llp_current = QMI_WDA_LINK_LAYER_PROTOCOL_UNKNOWN;

    /* Go on to next step */
    ctx->step++;
    internal_setup_data_format_context_step (task);
}

static void
sync_wda_data_format (GTask *task)
{
    MMPortQmi                                  *self;
    InternalSetupDataFormatContext             *ctx;
    g_autoptr(QmiMessageWdaSetDataFormatInput)  input = NULL;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    if (ctx->wda_llp_current != ctx->wda_llp_requested)
        mm_obj_dbg (self, "Updating device link layer protocol: %s -> %s",
                    qmi_wda_link_layer_protocol_get_string (ctx->wda_llp_current),
                    qmi_wda_link_layer_protocol_get_string (ctx->wda_llp_requested));

    input = qmi_message_wda_set_data_format_input_new ();
    qmi_message_wda_set_data_format_input_set_link_layer_protocol (input, ctx->wda_llp_requested, NULL);
    qmi_message_wda_set_data_format_input_set_uplink_data_aggregation_protocol (input, QMI_WDA_DATA_AGGREGATION_PROTOCOL_DISABLED, NULL);
    qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_protocol (input, QMI_WDA_DATA_AGGREGATION_PROTOCOL_DISABLED, NULL);
    if (ctx->use_endpoint)
        qmi_message_wda_set_data_format_input_set_endpoint_info (input, self->priv->endpoint_type, self->priv->endpoint_interface_number, NULL);

    qmi_client_wda_set_data_format (QMI_CLIENT_WDA (ctx->wda),
                                    input,
                                    10,
                                    g_task_get_cancellable (task),
                                    (GAsyncReadyCallback) set_data_format_ready,
                                    task);
}

static gboolean
setup_data_format_completed (GTask *task)
{
    InternalSetupDataFormatContext *ctx;

    ctx = g_task_get_task_data (task);

    /* check whether the current and requested ones are the same */
    if ((ctx->kernel_data_format_current == ctx->kernel_data_format_requested) &&
        (ctx->wda_llp_current            == ctx->wda_llp_requested)) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return TRUE;
    }

    return FALSE;
}

static void
check_data_format (GTask *task)
{
    MMPortQmi              *self;
    InternalSetupDataFormatContext *ctx;
    gboolean                first_iteration;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    first_iteration = (ctx->data_format_combination_i < 0);
    if (!first_iteration && setup_data_format_completed (task))
        return;

    /* go on to the next supported combination */
    for (++ctx->data_format_combination_i;
         ctx->data_format_combination_i <= (gint)G_N_ELEMENTS (data_format_combinations);
         ctx->data_format_combination_i++) {
        const DataFormatCombination *combination;

        combination = &data_format_combinations[ctx->data_format_combination_i];

        if ((combination->kernel_data_format == QMI_DEVICE_EXPECTED_DATA_FORMAT_802_3) &&
            !ctx->kernel_data_format_802_3_supported)
            continue;
        if ((combination->kernel_data_format == QMI_DEVICE_EXPECTED_DATA_FORMAT_RAW_IP) &&
            !ctx->kernel_data_format_raw_ip_supported)
            continue;

        mm_obj_dbg (self, "selected data format setup:");
        mm_obj_dbg (self, "    kernel format: %s", qmi_device_expected_data_format_get_string (combination->kernel_data_format));
        mm_obj_dbg (self, "    link layer protocol: %s", qmi_wda_link_layer_protocol_get_string (combination->wda_llp));

        ctx->kernel_data_format_requested = combination->kernel_data_format;
        ctx->wda_llp_requested            = combination->wda_llp;

        if (first_iteration && setup_data_format_completed (task))
            return;

        /* Go on to next step */
        ctx->step++;
        internal_setup_data_format_context_step (task);
        return;
    }

    g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "No more data format combinations supported");
    g_object_unref (task);
}

static gboolean
process_data_format_output (MMPortQmi                         *self,
                            QmiMessageWdaGetDataFormatOutput  *output,
                            InternalSetupDataFormatContext    *ctx,
                            GError                           **error)
{
    /* Let's consider the lack o the LLP TLV a hard error; it really would be strange
     * a module supporting WDA Get Data Format but not containing the LLP info */
    if (!qmi_message_wda_get_data_format_output_get_link_layer_protocol (output, &ctx->wda_llp_current, error))
        return FALSE;

    return TRUE;
}

static void
get_data_format_ready (QmiClientWda *client,
                       GAsyncResult *res,
                       GTask        *task)
{
    MMPortQmi                                   *self;
    InternalSetupDataFormatContext              *ctx;
    g_autoptr(QmiMessageWdaGetDataFormatOutput)  output = NULL;
    g_autoptr(GError)                            error = NULL;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    output = qmi_client_wda_get_data_format_finish (client, res, &error);
    if (!output ||
        !qmi_message_wda_get_data_format_output_get_result (output, &error) ||
        !process_data_format_output (self, output, ctx, &error)) {
        /* A 'missing argument' error when querying data format is seen in new
         * devices like the Quectel RM500Q, requiring the 'endpoint info' TLV.
         * When this happens, retry the step with the missing TLV.
         *
         * Note that this is not an additional step, we're still in the
         * GET_WDA_DATA_FORMAT step.
         */
        if (g_error_matches (error, QMI_PROTOCOL_ERROR, QMI_PROTOCOL_ERROR_MISSING_ARGUMENT) &&
            (self->priv->endpoint_type != QMI_DATA_ENDPOINT_TYPE_UNDEFINED)) {
            /* retry same step with endpoint info */
            ctx->use_endpoint = TRUE;
            internal_setup_data_format_context_step (task);
            return;
        }

        /* otherwise, fatal */
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    /* Go on to next step */
    ctx->step++;
    internal_setup_data_format_context_step (task);
}

static void
allocate_client_wda_ready (QmiDevice    *device,
                           GAsyncResult *res,
                           GTask        *task)
{
    InternalSetupDataFormatContext *ctx;
    GError                         *error = NULL;

    ctx = g_task_get_task_data (task);

    ctx->wda = qmi_device_allocate_client_finish (device, res, &error);
    if (!ctx->wda) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on to next step */
    ctx->step++;
    internal_setup_data_format_context_step (task);
}

static void
internal_setup_data_format_context_step (GTask *task)
{
    MMPortQmi                      *self;
    InternalSetupDataFormatContext *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    switch (ctx->step) {
        case INTERNAL_SETUP_DATA_FORMAT_STEP_FIRST:
            ctx->step++;
            /* Fall through */

        case INTERNAL_SETUP_DATA_FORMAT_STEP_KERNEL_DATA_FORMAT_CAPABILITIES:
            /* Load kernel data format capabilities, only on first loop iteration */
            load_kernel_data_format_capabilities (self,
                                                  ctx->device,
                                                  &ctx->kernel_data_format_802_3_supported,
                                                  &ctx->kernel_data_format_raw_ip_supported);
            ctx->step++;
            /* Fall through */

        case INTERNAL_SETUP_DATA_FORMAT_STEP_RETRY:
            ctx->step++;
            /* Fall through */

        case INTERNAL_SETUP_DATA_FORMAT_STEP_KERNEL_DATA_FORMAT_CURRENT:
            /* Only reload kernel data format if it was updated or on first loop */
            if (ctx->kernel_data_format_current == QMI_DEVICE_EXPECTED_DATA_FORMAT_UNKNOWN) {
                ctx->kernel_data_format_current = load_kernel_data_format_current (self, ctx->device);
            }
            ctx->step++;
            /* Fall through */

        case INTERNAL_SETUP_DATA_FORMAT_STEP_ALLOCATE_WDA_CLIENT:
            /* Only allocate new WDA client on first loop */
            if (ctx->data_format_combination_i < 0) {
                g_assert (!ctx->wda);
                qmi_device_allocate_client (ctx->device,
                                            QMI_SERVICE_WDA,
                                            QMI_CID_NONE,
                                            10,
                                            g_task_get_cancellable (task),
                                            (GAsyncReadyCallback) allocate_client_wda_ready,
                                            task);
                return;
            }
            ctx->step++;
            /* Fall through */

        case INTERNAL_SETUP_DATA_FORMAT_STEP_GET_WDA_DATA_FORMAT:
            /* Only reload WDA data format if it was updated or on first loop */
            if (ctx->wda_llp_current == QMI_WDA_LINK_LAYER_PROTOCOL_UNKNOWN) {
                g_autoptr(QmiMessageWdaGetDataFormatInput) input = NULL;

                if (ctx->use_endpoint) {
                    input = qmi_message_wda_get_data_format_input_new ();
                    qmi_message_wda_get_data_format_input_set_endpoint_info (input,
                                                                             self->priv->endpoint_type,
                                                                             self->priv->endpoint_interface_number,
                                                                             NULL);
                }
                qmi_client_wda_get_data_format (QMI_CLIENT_WDA (ctx->wda),
                                                input,
                                                10,
                                                g_task_get_cancellable (task),
                                                (GAsyncReadyCallback) get_data_format_ready,
                                                task);
                return;
            }
            ctx->step++;
            /* Fall through */

        case INTERNAL_SETUP_DATA_FORMAT_STEP_QUERY_DONE:
            mm_obj_dbg (self, "current data format setup:");
            mm_obj_dbg (self, "    kernel format: %s", qmi_device_expected_data_format_get_string (ctx->kernel_data_format_current));
            mm_obj_dbg (self, "    link layer protocol: %s", qmi_wda_link_layer_protocol_get_string (ctx->wda_llp_current));

            if (ctx->action == MM_PORT_QMI_SETUP_DATA_FORMAT_ACTION_QUERY) {
                g_task_return_boolean (task, TRUE);
                g_object_unref (task);
                return;
            }

            ctx->step++;
            /* Fall through */

        case INTERNAL_SETUP_DATA_FORMAT_STEP_CHECK_DATA_FORMAT:
            /* This step is the one that may complete the async operation
             * successfully */
            check_data_format (task);
            return;

        case INTERNAL_SETUP_DATA_FORMAT_STEP_SYNC_WDA_DATA_FORMAT:
            if (ctx->wda_llp_current != ctx->wda_llp_requested) {
                sync_wda_data_format (task);
                return;
            }
            ctx->step++;
            /* Fall through */

        case INTERNAL_SETUP_DATA_FORMAT_STEP_SYNC_KERNEL_DATA_FORMAT:
            if (ctx->kernel_data_format_current != ctx->kernel_data_format_requested) {
                sync_kernel_data_format (task);
                return;
            }
            ctx->step++;
            /* Fall through */

        case INTERNAL_SETUP_DATA_FORMAT_STEP_LAST:
            /* jump back to first step to reload current state after
             * the updates have been done */
            ctx->step = INTERNAL_SETUP_DATA_FORMAT_STEP_RETRY;
            internal_setup_data_format_context_step (task);
            return;

        default:
            g_assert_not_reached ();
    }
}

static void
internal_setup_data_format (MMPortQmi                      *self,
                            QmiDevice                      *device,
                            MMPortQmiSetupDataFormatAction  action,
                            GAsyncReadyCallback             callback,
                            gpointer                        user_data)
{
    InternalSetupDataFormatContext *ctx;
    GTask                          *task;

    task = g_task_new (self, NULL, callback, user_data);

    if (!device) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
                                 "Port must be open to setup data format");
        g_object_unref (task);
        return;
    }

    if (self->priv->wda_unsupported) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "Setting up data format is not supported");
        g_object_unref (task);
        return;
    }

    ctx = g_slice_new0 (InternalSetupDataFormatContext);
    ctx->device = g_object_ref (device);
    ctx->action = action;
    ctx->step = INTERNAL_SETUP_DATA_FORMAT_STEP_FIRST;
    ctx->data_format_combination_i = -1;
    ctx->kernel_data_format_current = QMI_DEVICE_EXPECTED_DATA_FORMAT_UNKNOWN;
    ctx->kernel_data_format_requested = QMI_DEVICE_EXPECTED_DATA_FORMAT_UNKNOWN;
    ctx->wda_llp_current = QMI_WDA_LINK_LAYER_PROTOCOL_UNKNOWN;
    ctx->wda_llp_requested = QMI_WDA_LINK_LAYER_PROTOCOL_UNKNOWN;
    g_task_set_task_data (task, ctx, (GDestroyNotify) internal_setup_data_format_context_free);

    internal_setup_data_format_context_step (task);
}

/*****************************************************************************/

gboolean
mm_port_qmi_setup_data_format_finish (MMPortQmi                *self,
                                      GAsyncResult             *res,
                                      GError                  **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
internal_setup_data_format_ready (MMPortQmi    *self,
                                  GAsyncResult *res,
                                  GTask        *task)
{
    GError *error = NULL;

    if (!internal_setup_data_format_finish (self,
                                            res,
                                            &self->priv->kernel_data_format,
                                            &self->priv->llp,
                                            &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_port_qmi_setup_data_format (MMPortQmi                      *self,
                               MMPortQmiSetupDataFormatAction  action,
                               GAsyncReadyCallback             callback,
                               gpointer                        user_data)
{
    GTask *task;

    /* External calls are never query */
    g_assert (action != MM_PORT_QMI_SETUP_DATA_FORMAT_ACTION_QUERY);

    task = g_task_new (self, NULL, callback, user_data);

    if (!self->priv->qmi_device) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE, "Port not open");
        g_object_unref (task);
        return;
    }

    if (self->priv->wda_unsupported) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED, "Setting up data format is unsupported");
        g_object_unref (task);
        return;
    }

    /* call internal method with the already open QmiDevice */
    internal_setup_data_format (self,
                                self->priv->qmi_device,
                                action,
                                (GAsyncReadyCallback)internal_setup_data_format_ready,
                                task);
}

/*****************************************************************************/

typedef enum {
    PORT_OPEN_STEP_FIRST,
    PORT_OPEN_STEP_CHECK_OPENING,
    PORT_OPEN_STEP_CHECK_ALREADY_OPEN,
    PORT_OPEN_STEP_DEVICE_NEW,
    PORT_OPEN_STEP_OPEN_WITHOUT_DATA_FORMAT,
    PORT_OPEN_STEP_SETUP_DATA_FORMAT,
    PORT_OPEN_STEP_CLOSE_BEFORE_OPEN_WITH_DATA_FORMAT,
    PORT_OPEN_STEP_OPEN_WITH_DATA_FORMAT,
    PORT_OPEN_STEP_LAST
} PortOpenStep;

typedef struct {
    QmiDevice                   *device;
    GError                      *error;
    PortOpenStep                 step;
    gboolean                     set_data_format;
    QmiDeviceExpectedDataFormat  kernel_data_format;
} PortOpenContext;

static void
port_open_context_free (PortOpenContext *ctx)
{
    g_assert (!ctx->error);
    g_clear_object (&ctx->device);
    g_slice_free (PortOpenContext, ctx);
}

gboolean
mm_port_qmi_open_finish (MMPortQmi     *self,
                         GAsyncResult  *res,
                         GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void port_open_step (GTask *task);

static void
port_open_complete_with_error (GTask *task)
{
    MMPortQmi       *self;
    PortOpenContext *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    g_assert (ctx->error);
    self->priv->in_progress = FALSE;
    g_task_return_error (task, g_steal_pointer (&ctx->error));
    g_object_unref (task);
}

static void
qmi_device_close_on_error_ready (QmiDevice    *qmi_device,
                                 GAsyncResult *res,
                                 GTask        *task)
{
    MMPortQmi         *self;
    g_autoptr(GError)  error = NULL;

    self = g_task_get_source_object (task);

    if (!qmi_device_close_finish (qmi_device, res, &error))
        mm_obj_warn (self, "Couldn't close QMI device after failed open sequence: %s", error->message);

    port_open_complete_with_error (task);
}

static void
qmi_device_open_second_ready (QmiDevice    *qmi_device,
                              GAsyncResult *res,
                              GTask        *task)
{
    MMPortQmi       *self;
    PortOpenContext *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    if (qmi_device_open_finish (qmi_device, res, &ctx->error)) {
        /* If the open with CTL data format is sucessful, update */
        self->priv->kernel_data_format = ctx->kernel_data_format;
        if (ctx->kernel_data_format == QMI_DEVICE_EXPECTED_DATA_FORMAT_RAW_IP)
            self->priv->llp = QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP;
        else if (ctx->kernel_data_format == QMI_DEVICE_EXPECTED_DATA_FORMAT_802_3)
            self->priv->llp = QMI_WDA_LINK_LAYER_PROTOCOL_802_3;
        else
            g_assert_not_reached ();
    }

    /* In both error and success, we go to last step */
    ctx->step++;
    port_open_step (task);
}

static void
qmi_device_close_to_reopen_ready (QmiDevice    *qmi_device,
                                  GAsyncResult *res,
                                  GTask        *task)
{
    MMPortQmi       *self;
    PortOpenContext *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    if (!qmi_device_close_finish (qmi_device, res, &ctx->error)) {
        mm_obj_warn (self, "Couldn't close QMI device to reopen it");
        ctx->step = PORT_OPEN_STEP_LAST;
    } else
        ctx->step++;
    port_open_step (task);
}

static void
open_internal_setup_data_format_ready (MMPortQmi    *self,
                                       GAsyncResult *res,
                                       GTask        *task)
{
    PortOpenContext   *ctx;
    g_autoptr(GError)  error = NULL;

    ctx = g_task_get_task_data (task);

    if (!internal_setup_data_format_finish (self,
                                            res,
                                            &self->priv->kernel_data_format,
                                            &self->priv->llp,
                                            &error)) {
        /* Continue with fallback to LLP requested via CTL */
        mm_obj_warn (self, "Couldn't setup data format: %s", error->message);
        self->priv->wda_unsupported = TRUE;
        ctx->step++;
    } else {
        /* on success, we're done */
        ctx->step = PORT_OPEN_STEP_LAST;
    }
    port_open_step (task);
}

static void
qmi_device_open_first_ready (QmiDevice    *qmi_device,
                             GAsyncResult *res,
                             GTask        *task)
{
    PortOpenContext *ctx;

    ctx = g_task_get_task_data (task);

    if (!qmi_device_open_finish (qmi_device, res, &ctx->error))
        /* Error opening the device */
        ctx->step = PORT_OPEN_STEP_LAST;
    else if (!ctx->set_data_format)
        /* If not setting data format, we're done */
        ctx->step = PORT_OPEN_STEP_LAST;
    else
        /* Go on to next step */
        ctx->step++;
    port_open_step (task);
}

static void
qmi_device_new_ready (GObject *unused,
                      GAsyncResult *res,
                      GTask *task)
{
    PortOpenContext *ctx;

    ctx = g_task_get_task_data (task);
    /* Store the device in the context until the operation is fully done,
     * so that we return IN_PROGRESS errors until we finish this async
     * operation. */
    ctx->device = qmi_device_new_finish (res, &ctx->error);
    if (!ctx->device)
        /* Error creating the device */
        ctx->step = PORT_OPEN_STEP_LAST;
    else
        /* Go on to next step */
        ctx->step++;
    port_open_step (task);
}

static void
port_open_step (GTask *task)
{
    MMPortQmi       *self;
    PortOpenContext *ctx;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);
    switch (ctx->step) {
    case PORT_OPEN_STEP_FIRST:
        mm_obj_dbg (self, "Opening QMI device...");
        ctx->step++;
        /* Fall through */

    case PORT_OPEN_STEP_CHECK_OPENING:
        mm_obj_dbg (self, "Checking if QMI device already opening...");
        if (self->priv->in_progress) {
            g_task_return_new_error (task,
                                     MM_CORE_ERROR,
                                     MM_CORE_ERROR_IN_PROGRESS,
                                     "QMI device open/close operation in progress");
            g_object_unref (task);
            return;
        }
        ctx->step++;
        /* Fall through */

    case PORT_OPEN_STEP_CHECK_ALREADY_OPEN:
        mm_obj_dbg (self, "Checking if QMI device already open...");
        if (self->priv->qmi_device) {
            g_task_return_boolean (task, TRUE);
            g_object_unref (task);
            return;
        }
        ctx->step++;
        /* Fall through */

    case PORT_OPEN_STEP_DEVICE_NEW: {
        GFile *file;
        gchar *fullpath;

        fullpath = g_strdup_printf ("/dev/%s", mm_port_get_device (MM_PORT (self)));
        file = g_file_new_for_path (fullpath);

        /* We flag in this point that we're opening. From now on, if we stop
         * for whatever reason, we should clear this flag. We do this by ensuring
         * that all callbacks go through the LAST step for completing. */
        self->priv->in_progress = TRUE;

        mm_obj_dbg (self, "Creating QMI device...");
        qmi_device_new (file,
                        g_task_get_cancellable (task),
                        (GAsyncReadyCallback) qmi_device_new_ready,
                        task);

        g_free (fullpath);
        g_object_unref (file);
        return;
    }

    case PORT_OPEN_STEP_OPEN_WITHOUT_DATA_FORMAT:
        if (!self->priv->wda_unsupported) {
            /* Now open the QMI device without any data format CTL flag */
            mm_obj_dbg (self, "Opening device without data format update...");
            qmi_device_open (ctx->device,
                             (QMI_DEVICE_OPEN_FLAGS_VERSION_INFO |
                              QMI_DEVICE_OPEN_FLAGS_PROXY),
                             25,
                             g_task_get_cancellable (task),
                             (GAsyncReadyCallback) qmi_device_open_first_ready,
                             task);
            return;
        }
        ctx->step++;
        /* Fall through */

    case PORT_OPEN_STEP_SETUP_DATA_FORMAT:
        if (qmi_device_is_open (ctx->device)) {
            internal_setup_data_format (self,
                                        ctx->device,
                                        MM_PORT_QMI_SETUP_DATA_FORMAT_ACTION_QUERY,
                                        (GAsyncReadyCallback) open_internal_setup_data_format_ready,
                                        task);
            return;
        }
        ctx->step++;
        /* Fall through */

    case PORT_OPEN_STEP_CLOSE_BEFORE_OPEN_WITH_DATA_FORMAT:
        /* This fallback only applies when WDA unsupported */
        if (qmi_device_is_open (ctx->device)) {
            mm_obj_dbg (self, "Closing device to reopen it right away...");
            qmi_device_close_async (ctx->device,
                                    5,
                                    g_task_get_cancellable (task),
                                    (GAsyncReadyCallback) qmi_device_close_to_reopen_ready,
                                    task);
            return;
        }
        ctx->step++;
        /* Fall through */

    case PORT_OPEN_STEP_OPEN_WITH_DATA_FORMAT: {
        QmiDeviceOpenFlags open_flags;

        /* Common open flags */
        open_flags = (QMI_DEVICE_OPEN_FLAGS_VERSION_INFO |
                      QMI_DEVICE_OPEN_FLAGS_PROXY        |
                      QMI_DEVICE_OPEN_FLAGS_NET_NO_QOS_HEADER);

        ctx->kernel_data_format = load_kernel_data_format_current (self, ctx->device);

        /* Need to reopen setting 802.3/raw-ip using CTL */
        if (ctx->kernel_data_format == QMI_DEVICE_EXPECTED_DATA_FORMAT_RAW_IP)
            open_flags |= QMI_DEVICE_OPEN_FLAGS_NET_RAW_IP;
        else if (ctx->kernel_data_format == QMI_DEVICE_EXPECTED_DATA_FORMAT_802_3)
            open_flags |= QMI_DEVICE_OPEN_FLAGS_NET_802_3;
        else {
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                     "Unexpected kernel data format: cannot setup using CTL");
            g_object_unref (task);
            return;
        }

        mm_obj_dbg (self, "Reopening device with data format: %s...",
                    qmi_device_expected_data_format_get_string (ctx->kernel_data_format));
        qmi_device_open (ctx->device,
                         open_flags,
                         10,
                         g_task_get_cancellable (task),
                         (GAsyncReadyCallback) qmi_device_open_second_ready,
                         task);
        return;
    }

    case PORT_OPEN_STEP_LAST:
        if (ctx->error) {
            mm_obj_dbg (self, "QMI port open operation failed: %s", ctx->error->message);

            if (ctx->device) {
                qmi_device_close_async (ctx->device,
                                        5,
                                        NULL,
                                        (GAsyncReadyCallback) qmi_device_close_on_error_ready,
                                        task);
                return;
            }

            port_open_complete_with_error (task);
            return;
        }

        mm_obj_dbg (self, "QMI port open operation finished successfully");

        /* Store device in private info */
        g_assert (ctx->device);
        g_assert (!self->priv->qmi_device);
        self->priv->qmi_device = g_object_ref (ctx->device);
        self->priv->in_progress = FALSE;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
}

void
mm_port_qmi_open (MMPortQmi           *self,
                  gboolean             set_data_format,
                  GCancellable        *cancellable,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    PortOpenContext *ctx;
    GTask           *task;

    ctx = g_slice_new0 (PortOpenContext);
    ctx->step = PORT_OPEN_STEP_FIRST;
    ctx->set_data_format = set_data_format;
    ctx->kernel_data_format = QMI_DEVICE_EXPECTED_DATA_FORMAT_UNKNOWN;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)port_open_context_free);
    port_open_step (task);
}

/*****************************************************************************/

gboolean
mm_port_qmi_is_open (MMPortQmi *self)
{
    g_return_val_if_fail (MM_IS_PORT_QMI (self), FALSE);

    return !!self->priv->qmi_device;
}

/*****************************************************************************/

typedef struct {
    QmiDevice *qmi_device;
} PortQmiCloseContext;

static void
port_qmi_close_context_free (PortQmiCloseContext *ctx)
{
    g_clear_object (&ctx->qmi_device);
    g_slice_free (PortQmiCloseContext, ctx);
}

gboolean
mm_port_qmi_close_finish (MMPortQmi     *self,
                          GAsyncResult  *res,
                          GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
qmi_device_close_ready (QmiDevice    *qmi_device,
                        GAsyncResult *res,
                        GTask        *task)
{
    GError    *error = NULL;
    MMPortQmi *self;

    self = g_task_get_source_object (task);

    g_assert (!self->priv->qmi_device);
    self->priv->in_progress = FALSE;

    if (!qmi_device_close_finish (qmi_device, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_port_qmi_close (MMPortQmi           *self,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    PortQmiCloseContext *ctx;
    GTask               *task;
    GList               *l;

    g_return_if_fail (MM_IS_PORT_QMI (self));

    task = g_task_new (self, NULL, callback, user_data);

    if (self->priv->in_progress) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_IN_PROGRESS,
                                 "QMI device open/close operation in progress");
        g_object_unref (task);
        return;
    }

    if (!self->priv->qmi_device) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    self->priv->in_progress = TRUE;

    /* Store device to close in the context */
    ctx = g_slice_new0 (PortQmiCloseContext);
    ctx->qmi_device = g_steal_pointer (&self->priv->qmi_device);
    g_task_set_task_data (task, ctx, (GDestroyNotify)port_qmi_close_context_free);

    /* Release all allocated clients */
    for (l = self->priv->services; l; l = g_list_next (l)) {
        ServiceInfo *info = l->data;

        mm_obj_dbg (self, "Releasing client for service '%s'...", qmi_service_get_string (info->service));
        qmi_device_release_client (ctx->qmi_device,
                                   info->client,
                                   QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID,
                                   3, NULL, NULL, NULL);
        g_clear_object (&info->client);
    }
    g_list_free_full (self->priv->services, g_free);
    self->priv->services = NULL;

    qmi_device_close_async (ctx->qmi_device,
                            5,
                            NULL,
                            (GAsyncReadyCallback)qmi_device_close_ready,
                            task);
}

/*****************************************************************************/

MMPortQmi *
mm_port_qmi_new (const gchar  *name,
                 MMPortSubsys  subsys)
{
    MMPortQmi *self;

    self = MM_PORT_QMI (g_object_new (MM_TYPE_PORT_QMI,
                                      MM_PORT_DEVICE, name,
                                      MM_PORT_SUBSYS, subsys,
                                      MM_PORT_TYPE, MM_PORT_TYPE_QMI,
                                      NULL));

    /* load endpoint info as soon as kernel device is set */
    self->priv->endpoint_info_signal_id = g_signal_connect (self,
                                                            "notify::" MM_PORT_KERNEL_DEVICE,
                                                            G_CALLBACK (initialize_endpoint_info),
                                                            NULL);

    return self;
}

static void
mm_port_qmi_init (MMPortQmi *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_PORT_QMI, MMPortQmiPrivate);
}

static void
dispose (GObject *object)
{
    MMPortQmi *self = MM_PORT_QMI (object);
    GList *l;

    if (self->priv->endpoint_info_signal_id) {
        g_signal_handler_disconnect (self, self->priv->endpoint_info_signal_id);
        self->priv->endpoint_info_signal_id = 0;
    }

    /* Deallocate all clients */
    for (l = self->priv->services; l; l = g_list_next (l)) {
        ServiceInfo *info = l->data;

        if (info->client)
            g_object_unref (info->client);
    }
    g_list_free_full (self->priv->services, g_free);
    self->priv->services = NULL;

    /* Clear device object */
    g_clear_object (&self->priv->qmi_device);

    G_OBJECT_CLASS (mm_port_qmi_parent_class)->dispose (object);
}

static void
mm_port_qmi_class_init (MMPortQmiClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMPortQmiPrivate));

    /* Virtual methods */
    object_class->dispose = dispose;
}
