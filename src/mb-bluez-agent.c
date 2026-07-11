#include "mb-bluez-agent.h"

#include "mb-bluez.h"
#include "mb-timeouts.h"

#include <string.h>

#define AGENT_MANAGER_IFACE "org.bluez.AgentManager1"
#define DAEMON_AGENT_PATH   "/org/midi_ble_rt/DaemonAgent"

static const char agent_xml[] =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='RequestPinCode'>"
    "      <arg name='device' type='o' direction='in'/>"
    "      <arg name='pincode' type='s' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPinCode'>"
    "      <arg name='device' type='o' direction='in'/>"
    "      <arg name='pincode' type='s' direction='in'/>"
    "    </method>"
    "    <method name='RequestPasskey'>"
    "      <arg name='device' type='o' direction='in'/>"
    "      <arg name='passkey' type='u' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPasskey'>"
    "      <arg name='device' type='o' direction='in'/>"
    "      <arg name='passkey' type='u' direction='in'/>"
    "      <arg name='entered' type='q' direction='in'/>"
    "    </method>"
    "    <method name='RequestConfirmation'>"
    "      <arg name='device' type='o' direction='in'/>"
    "      <arg name='passkey' type='u' direction='in'/>"
    "    </method>"
    "    <method name='RequestAuthorization'>"
    "      <arg name='device' type='o' direction='in'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg name='device' type='o' direction='in'/>"
    "      <arg name='uuid' type='s' direction='in'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

static void agent_method_call(GDBusConnection *connection,
                              const char *sender,
                              const char *object_path,
                              const char *interface_name,
                              const char *method_name,
                              GVariant *parameters,
                              GDBusMethodInvocation *invocation,
                              gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "Release") == 0 ||
        g_strcmp0(method_name, "Cancel") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "RequestPinCode") == 0) {
        const char *device = NULL;
        g_variant_get(parameters, "(&o)", &device);
        g_print("BlueZ Agent1.RequestPinCode for %s; returning 0000\n",
                device ? device : "-");
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(s)", "0000"));
        return;
    }

    if (g_strcmp0(method_name, "DisplayPinCode") == 0) {
        const char *device = NULL;
        const char *pin = NULL;
        g_variant_get(parameters, "(&o&s)", &device, &pin);
        g_print("BlueZ Agent1.DisplayPinCode for %s: %s\n",
                device ? device : "-",
                pin ? pin : "-");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "RequestPasskey") == 0) {
        const char *device = NULL;
        g_variant_get(parameters, "(&o)", &device);
        g_print("BlueZ Agent1.RequestPasskey for %s; returning 000000\n",
                device ? device : "-");
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(u)", 0u));
        return;
    }

    if (g_strcmp0(method_name, "DisplayPasskey") == 0) {
        const char *device = NULL;
        guint32 passkey = 0;
        guint16 entered = 0;
        g_variant_get(parameters, "(&ouq)", &device, &passkey, &entered);
        g_print("BlueZ Agent1.DisplayPasskey for %s: %06u entered=%u\n",
                device ? device : "-",
                passkey,
                entered);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "RequestConfirmation") == 0) {
        const char *device = NULL;
        guint32 passkey = 0;
        g_variant_get(parameters, "(&ou)", &device, &passkey);
        g_print("BlueZ Agent1.RequestConfirmation for %s: %06u accepted\n",
                device ? device : "-",
                passkey);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "RequestAuthorization") == 0) {
        const char *device = NULL;
        g_variant_get(parameters, "(&o)", &device);
        g_print("BlueZ Agent1.RequestAuthorization for %s accepted\n",
                device ? device : "-");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "AuthorizeService") == 0) {
        const char *device = NULL;
        const char *uuid = NULL;
        g_variant_get(parameters, "(&o&s)", &device, &uuid);
        g_print("BlueZ Agent1.AuthorizeService for %s uuid=%s accepted\n",
                device ? device : "-",
                uuid ? uuid : "-");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    g_dbus_method_invocation_return_error(invocation,
                                          G_IO_ERROR,
                                          G_IO_ERROR_NOT_SUPPORTED,
                                          "Unsupported Agent1 method: %s",
                                          method_name ? method_name : "-");
}

static const GDBusInterfaceVTable agent_vtable = {
    .method_call = agent_method_call,
    .get_property = NULL,
    .set_property = NULL,
};

bool mb_bluez_agent_start(MbBluezAgent *agent,
                          GDBusConnection *system_bus,
                          GError **error) {
    if (!agent || !system_bus) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "invalid BlueZ Agent1 start request");
        return false;
    }

    if (agent->registration_id != 0)
        return true;

    memset(agent, 0, sizeof(*agent));
    agent->bus = g_object_ref(system_bus);

    agent->node_info = g_dbus_node_info_new_for_xml(agent_xml, error);
    if (!agent->node_info)
        goto fail;

    agent->registration_id =
        g_dbus_connection_register_object(agent->bus,
                                          DAEMON_AGENT_PATH,
                                          agent->node_info->interfaces[0],
                                          &agent_vtable,
                                          agent,
                                          NULL,
                                          error);
    if (agent->registration_id == 0)
        goto fail;

    GVariant *reply =
        g_dbus_connection_call_sync(agent->bus,
                                    BLUEZ_BUS,
                                    "/org/bluez",
                                    AGENT_MANAGER_IFACE,
                                    "RegisterAgent",
                                    g_variant_new("(os)",
                                                  DAEMON_AGENT_PATH,
                                                  "NoInputNoOutput"),
                                    NULL,
                                    G_DBUS_CALL_FLAGS_NONE,
                                    MB_BLUEZ_PAIR_TIMEOUT_MS,
                                    NULL,
                                    error);
    if (!reply)
        goto fail;

    g_variant_unref(reply);
    agent->registered_with_bluez = true;
    g_print("BlueZ Agent1 registered for midi-ble-rtd at %s.\n",
            DAEMON_AGENT_PATH);
    return true;

fail:
    mb_bluez_agent_stop(agent);
    return false;
}

void mb_bluez_agent_stop(MbBluezAgent *agent) {
    if (!agent)
        return;

    if (agent->registered_with_bluez && agent->bus) {
        GError *error = NULL;
        GVariant *reply =
            g_dbus_connection_call_sync(agent->bus,
                                        BLUEZ_BUS,
                                        "/org/bluez",
                                        AGENT_MANAGER_IFACE,
                                        "UnregisterAgent",
                                        g_variant_new("(o)", DAEMON_AGENT_PATH),
                                        NULL,
                                        G_DBUS_CALL_FLAGS_NONE,
                                        MB_BLUEZ_PAIR_TIMEOUT_MS,
                                        NULL,
                                        &error);
        if (reply)
            g_variant_unref(reply);
        else
            g_clear_error(&error);

        agent->registered_with_bluez = false;
    }

    if (agent->registration_id && agent->bus) {
        g_dbus_connection_unregister_object(agent->bus,
                                            agent->registration_id);
        agent->registration_id = 0;
    }

    g_clear_pointer(&agent->node_info, g_dbus_node_info_unref);
    g_clear_object(&agent->bus);
}
