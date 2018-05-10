/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering
***/

#include "sd-bus.h"

#include "alloc-util.h"
#include "bpf-firewall.h"
#include "bus-common-errors.h"
#include "cgroup-util.h"
#include "condition.h"
#include "dbus-job.h"
#include "dbus-unit.h"
#include "dbus-util.h"
#include "dbus.h"
#include "fd-util.h"
#include "locale-util.h"
#include "log.h"
#include "path-util.h"
#include "process-util.h"
#include "selinux-access.h"
#include "signal-util.h"
#include "special.h"
#include "string-util.h"
#include "strv.h"
#include "user-util.h"
#include "web-util.h"

static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_collect_mode, collect_mode, CollectMode);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_load_state, unit_load_state, UnitLoadState);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_job_mode, job_mode, JobMode);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_emergency_action, emergency_action, EmergencyAction);

static int property_get_names(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;
        Iterator i;
        const char *t;
        int r;

        assert(bus);
        assert(reply);
        assert(u);

        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
                return r;

        SET_FOREACH(t, u->names, i) {
                r = sd_bus_message_append(reply, "s", t);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_following(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata, *f;

        assert(bus);
        assert(reply);
        assert(u);

        f = unit_following(u);
        return sd_bus_message_append(reply, "s", f ? f->id : NULL);
}

static int property_get_dependencies(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Hashmap *h = *(Hashmap**) userdata;
        Iterator j;
        Unit *u;
        void *v;
        int r;

        assert(bus);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
                return r;

        HASHMAP_FOREACH_KEY(v, u, h, j) {
                r = sd_bus_message_append(reply, "s", u->id);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_obsolete_dependencies(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        assert(bus);
        assert(reply);

        /* For dependency types we don't support anymore always return an empty array */
        return sd_bus_message_append(reply, "as", 0);
}

static int property_get_requires_mounts_for(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Hashmap *h = *(Hashmap**) userdata;
        const char *p;
        Iterator j;
        void *v;
        int r;

        assert(bus);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
                return r;

        HASHMAP_FOREACH_KEY(v, p, h, j) {
                r = sd_bus_message_append(reply, "s", p);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_description(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "s", unit_description(u));
}

static int property_get_active_state(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "s", unit_active_state_to_string(unit_active_state(u)));
}

static int property_get_sub_state(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "s", unit_sub_state_to_string(u));
}

static int property_get_unit_file_preset(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;
        int r;

        assert(bus);
        assert(reply);
        assert(u);

        r = unit_get_unit_file_preset(u);

        return sd_bus_message_append(reply, "s",
                                     r < 0 ? NULL:
                                     r > 0 ? "enabled" : "disabled");
}

static int property_get_unit_file_state(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "s", unit_file_state_to_string(unit_get_unit_file_state(u)));
}

static int property_get_can_start(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "b", unit_can_start(u) && !u->refuse_manual_start);
}

static int property_get_can_stop(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "b", unit_can_stop(u) && !u->refuse_manual_stop);
}

static int property_get_can_reload(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "b", unit_can_reload(u));
}

static int property_get_can_isolate(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "b", unit_can_isolate(u) && !u->refuse_manual_start);
}

static int property_get_job(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        _cleanup_free_ char *p = NULL;
        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        if (!u->job)
                return sd_bus_message_append(reply, "(uo)", 0, "/");

        p = job_dbus_path(u->job);
        if (!p)
                return -ENOMEM;

        return sd_bus_message_append(reply, "(uo)", u->job->id, p);
}

static int property_get_need_daemon_reload(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "b", unit_need_daemon_reload(u));
}

static int property_get_conditions(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        const char *(*to_string)(ConditionType type) = NULL;
        Condition **list = userdata, *c;
        int r;

        assert(bus);
        assert(reply);
        assert(list);

        to_string = streq(property, "Asserts") ? assert_type_to_string : condition_type_to_string;

        r = sd_bus_message_open_container(reply, 'a', "(sbbsi)");
        if (r < 0)
                return r;

        LIST_FOREACH(conditions, c, *list) {
                int tristate;

                tristate =
                        c->result == CONDITION_UNTESTED ? 0 :
                        c->result == CONDITION_SUCCEEDED ? 1 : -1;

                r = sd_bus_message_append(reply, "(sbbsi)",
                                          to_string(c->type),
                                          c->trigger, c->negate,
                                          c->parameter, tristate);
                if (r < 0)
                        return r;

        }

        return sd_bus_message_close_container(reply);
}

static int property_get_load_error(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        _cleanup_(sd_bus_error_free) sd_bus_error e = SD_BUS_ERROR_NULL;
        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        if (u->load_error != 0)
                sd_bus_error_set_errno(&e, u->load_error);

        return sd_bus_message_append(reply, "(ss)", e.name, e.message);
}

static int bus_verify_manage_units_async_full(
                Unit *u,
                const char *verb,
                int capability,
                const char *polkit_message,
                bool interactive,
                sd_bus_message *call,
                sd_bus_error *error) {

        const char *details[9] = {
                "unit", u->id,
                "verb", verb,
        };

        if (polkit_message) {
                details[4] = "polkit.message";
                details[5] = polkit_message;
                details[6] = "polkit.gettext_domain";
                details[7] = GETTEXT_PACKAGE;
        }

        return bus_verify_polkit_async(
                        call,
                        capability,
                        "org.freedesktop.systemd1.manage-units",
                        details,
                        interactive,
                        UID_INVALID,
                        &u->manager->polkit_registry,
                        error);
}

int bus_unit_method_start_generic(
                sd_bus_message *message,
                Unit *u,
                JobType job_type,
                bool reload_if_possible,
                sd_bus_error *error) {

        const char *smode;
        JobMode mode;
        _cleanup_free_ char *verb = NULL;
        static const char *const polkit_message_for_job[_JOB_TYPE_MAX] = {
                [JOB_START]       = N_("Authentication is required to start '$(unit)'."),
                [JOB_STOP]        = N_("Authentication is required to stop '$(unit)'."),
                [JOB_RELOAD]      = N_("Authentication is required to reload '$(unit)'."),
                [JOB_RESTART]     = N_("Authentication is required to restart '$(unit)'."),
                [JOB_TRY_RESTART] = N_("Authentication is required to restart '$(unit)'."),
        };
        int r;

        assert(message);
        assert(u);
        assert(job_type >= 0 && job_type < _JOB_TYPE_MAX);

        r = mac_selinux_unit_access_check(
                        u, message,
                        job_type_to_access_method(job_type),
                        error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "s", &smode);
        if (r < 0)
                return r;

        mode = job_mode_from_string(smode);
        if (mode < 0)
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Job mode %s invalid", smode);

        if (reload_if_possible)
                verb = strjoin("reload-or-", job_type_to_string(job_type));
        else
                verb = strdup(job_type_to_string(job_type));
        if (!verb)
                return -ENOMEM;

        r = bus_verify_manage_units_async_full(
                        u,
                        verb,
                        CAP_SYS_ADMIN,
                        job_type < _JOB_TYPE_MAX ? polkit_message_for_job[job_type] : NULL,
                        true,
                        message,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* No authorization for now, but the async polkit stuff will call us again when it has it */

        return bus_unit_queue_job(message, u, job_type, mode, reload_if_possible, error);
}

static int method_start(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return bus_unit_method_start_generic(message, userdata, JOB_START, false, error);
}

static int method_stop(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return bus_unit_method_start_generic(message, userdata, JOB_STOP, false, error);
}

static int method_reload(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return bus_unit_method_start_generic(message, userdata, JOB_RELOAD, false, error);
}

static int method_restart(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return bus_unit_method_start_generic(message, userdata, JOB_RESTART, false, error);
}

static int method_try_restart(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return bus_unit_method_start_generic(message, userdata, JOB_TRY_RESTART, false, error);
}

static int method_reload_or_restart(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return bus_unit_method_start_generic(message, userdata, JOB_RESTART, true, error);
}

static int method_reload_or_try_restart(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return bus_unit_method_start_generic(message, userdata, JOB_TRY_RESTART, true, error);
}

int bus_unit_method_kill(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Unit *u = userdata;
        const char *swho;
        int32_t signo;
        KillWho who;
        int r;

        assert(message);
        assert(u);

        r = mac_selinux_unit_access_check(u, message, "stop", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "si", &swho, &signo);
        if (r < 0)
                return r;

        if (isempty(swho))
                who = KILL_ALL;
        else {
                who = kill_who_from_string(swho);
                if (who < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid who argument %s", swho);
        }

        if (!SIGNAL_VALID(signo))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Signal number out of range.");

        r = bus_verify_manage_units_async_full(
                        u,
                        "kill",
                        CAP_KILL,
                        N_("Authentication is required to kill '$(unit)'."),
                        true,
                        message,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* No authorization for now, but the async polkit stuff will call us again when it has it */

        r = unit_kill(u, who, signo, error);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, NULL);
}

int bus_unit_method_reset_failed(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Unit *u = userdata;
        int r;

        assert(message);
        assert(u);

        r = mac_selinux_unit_access_check(u, message, "reload", error);
        if (r < 0)
                return r;

        r = bus_verify_manage_units_async_full(
                        u,
                        "reset-failed",
                        CAP_SYS_ADMIN,
                        N_("Authentication is required to reset the \"failed\" state of '$(unit)'."),
                        true,
                        message,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* No authorization for now, but the async polkit stuff will call us again when it has it */

        unit_reset_failed(u);

        return sd_bus_reply_method_return(message, NULL);
}

int bus_unit_method_set_properties(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Unit *u = userdata;
        int runtime, r;

        assert(message);
        assert(u);

        r = mac_selinux_unit_access_check(u, message, "start", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "b", &runtime);
        if (r < 0)
                return r;

        r = bus_verify_manage_units_async_full(
                        u,
                        "set-property",
                        CAP_SYS_ADMIN,
                        N_("Authentication is required to set properties on '$(unit)'."),
                        true,
                        message,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* No authorization for now, but the async polkit stuff will call us again when it has it */

        r = bus_unit_set_properties(u, message, runtime ? UNIT_RUNTIME : UNIT_PERSISTENT, true, error);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, NULL);
}

int bus_unit_method_ref(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Unit *u = userdata;
        int r;

        assert(message);
        assert(u);

        r = mac_selinux_unit_access_check(u, message, "start", error);
        if (r < 0)
                return r;

        r = bus_verify_manage_units_async_full(
                        u,
                        "ref",
                        CAP_SYS_ADMIN,
                        NULL,
                        false,
                        message,
                        error);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* No authorization for now, but the async polkit stuff will call us again when it has it */

        r = bus_unit_track_add_sender(u, message);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, NULL);
}

int bus_unit_method_unref(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Unit *u = userdata;
        int r;

        assert(message);
        assert(u);

        r = bus_unit_track_remove_sender(u, message);
        if (r == -EUNATCH)
                return sd_bus_error_setf(error, BUS_ERROR_NOT_REFERENCED, "Unit has not been referenced yet.");
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, NULL);
}

const sd_bus_vtable bus_unit_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_PROPERTY("Id", "s", NULL, offsetof(Unit, id), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Names", "as", property_get_names, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Following", "s", property_get_following, 0, 0),
        SD_BUS_PROPERTY("Requires", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_REQUIRES]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Requisite", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_REQUISITE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Wants", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_WANTS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("BindsTo", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_BINDS_TO]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PartOf", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_PART_OF]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RequiredBy", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_REQUIRED_BY]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RequisiteOf", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_REQUISITE_OF]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("WantedBy", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_WANTED_BY]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("BoundBy", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_BOUND_BY]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ConsistsOf", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_CONSISTS_OF]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Conflicts", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_CONFLICTS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ConflictedBy", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_CONFLICTED_BY]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Before", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_BEFORE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("After", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_AFTER]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("OnFailure", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_ON_FAILURE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Triggers", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_TRIGGERS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TriggeredBy", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_TRIGGERED_BY]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PropagatesReloadTo", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_PROPAGATES_RELOAD_TO]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ReloadPropagatedFrom", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_RELOAD_PROPAGATED_FROM]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("JoinsNamespaceOf", "as", property_get_dependencies, offsetof(Unit, dependencies[UNIT_JOINS_NAMESPACE_OF]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RequiresMountsFor", "as", property_get_requires_mounts_for, offsetof(Unit, requires_mounts_for), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Documentation", "as", NULL, offsetof(Unit, documentation), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Description", "s", property_get_description, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LoadState", "s", property_get_load_state, offsetof(Unit, load_state), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ActiveState", "s", property_get_active_state, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("SubState", "s", property_get_sub_state, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("FragmentPath", "s", NULL, offsetof(Unit, fragment_path), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SourcePath", "s", NULL, offsetof(Unit, source_path), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("DropInPaths", "as", NULL, offsetof(Unit, dropin_paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UnitFileState", "s", property_get_unit_file_state, 0, 0),
        SD_BUS_PROPERTY("UnitFilePreset", "s", property_get_unit_file_preset, 0, 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("StateChangeTimestamp", offsetof(Unit, state_change_timestamp), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        BUS_PROPERTY_DUAL_TIMESTAMP("InactiveExitTimestamp", offsetof(Unit, inactive_exit_timestamp), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        BUS_PROPERTY_DUAL_TIMESTAMP("ActiveEnterTimestamp", offsetof(Unit, active_enter_timestamp), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        BUS_PROPERTY_DUAL_TIMESTAMP("ActiveExitTimestamp", offsetof(Unit, active_exit_timestamp), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        BUS_PROPERTY_DUAL_TIMESTAMP("InactiveEnterTimestamp", offsetof(Unit, inactive_enter_timestamp), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("CanStart", "b", property_get_can_start, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanStop", "b", property_get_can_stop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanReload", "b", property_get_can_reload, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanIsolate", "b", property_get_can_isolate, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Job", "(uo)", property_get_job, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("StopWhenUnneeded", "b", bus_property_get_bool, offsetof(Unit, stop_when_unneeded), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RefuseManualStart", "b", bus_property_get_bool, offsetof(Unit, refuse_manual_start), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RefuseManualStop", "b", bus_property_get_bool, offsetof(Unit, refuse_manual_stop), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("AllowIsolate", "b", bus_property_get_bool, offsetof(Unit, allow_isolate), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("DefaultDependencies", "b", bus_property_get_bool, offsetof(Unit, default_dependencies), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("OnFailureJobMode", "s", property_get_job_mode, offsetof(Unit, on_failure_job_mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("IgnoreOnIsolate", "b", bus_property_get_bool, offsetof(Unit, ignore_on_isolate), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NeedDaemonReload", "b", property_get_need_daemon_reload, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("JobTimeoutUSec", "t", bus_property_get_usec, offsetof(Unit, job_timeout), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("JobRunningTimeoutUSec", "t", bus_property_get_usec, offsetof(Unit, job_running_timeout), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("JobTimeoutAction", "s", property_get_emergency_action, offsetof(Unit, job_timeout_action), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("JobTimeoutRebootArgument", "s", NULL, offsetof(Unit, job_timeout_reboot_arg), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ConditionResult", "b", bus_property_get_bool, offsetof(Unit, condition_result), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("AssertResult", "b", bus_property_get_bool, offsetof(Unit, assert_result), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        BUS_PROPERTY_DUAL_TIMESTAMP("ConditionTimestamp", offsetof(Unit, condition_timestamp), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        BUS_PROPERTY_DUAL_TIMESTAMP("AssertTimestamp", offsetof(Unit, assert_timestamp), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Conditions", "a(sbbsi)", property_get_conditions, offsetof(Unit, conditions), 0),
        SD_BUS_PROPERTY("Asserts", "a(sbbsi)", property_get_conditions, offsetof(Unit, asserts), 0),
        SD_BUS_PROPERTY("LoadError", "(ss)", property_get_load_error, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Transient", "b", bus_property_get_bool, offsetof(Unit, transient), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Perpetual", "b", bus_property_get_bool, offsetof(Unit, perpetual), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StartLimitIntervalUSec", "t", bus_property_get_usec, offsetof(Unit, start_limit.interval), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StartLimitBurst", "u", bus_property_get_unsigned, offsetof(Unit, start_limit.burst), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StartLimitAction", "s", property_get_emergency_action, offsetof(Unit, start_limit_action), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("FailureAction", "s", property_get_emergency_action, offsetof(Unit, failure_action), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SuccessAction", "s", property_get_emergency_action, offsetof(Unit, success_action), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RebootArgument", "s", NULL, offsetof(Unit, reboot_arg), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("InvocationID", "ay", bus_property_get_id128, offsetof(Unit, invocation_id), 0),
        SD_BUS_PROPERTY("CollectMode", "s", property_get_collect_mode, offsetof(Unit, collect_mode), 0),

        SD_BUS_METHOD("Start", "s", "o", method_start, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Stop", "s", "o", method_stop, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Reload", "s", "o", method_reload, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Restart", "s", "o", method_restart, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("TryRestart", "s", "o", method_try_restart, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("ReloadOrRestart", "s", "o", method_reload_or_restart, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("ReloadOrTryRestart", "s", "o", method_reload_or_try_restart, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Kill", "si", NULL, bus_unit_method_kill, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("ResetFailed", NULL, NULL, bus_unit_method_reset_failed, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SetProperties", "ba(sv)", NULL, bus_unit_method_set_properties, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Ref", NULL, NULL, bus_unit_method_ref, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Unref", NULL, NULL, bus_unit_method_unref, SD_BUS_VTABLE_UNPRIVILEGED),

        /* Obsolete properties or obsolete alias names */
        SD_BUS_PROPERTY("RequiresOverridable", "as", property_get_obsolete_dependencies, 0, SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("RequisiteOverridable", "as", property_get_obsolete_dependencies, 0, SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("RequiredByOverridable", "as", property_get_obsolete_dependencies, 0, SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("RequisiteOfOverridable", "as", property_get_obsolete_dependencies, 0, SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("StartLimitInterval", "t", bus_property_get_usec, offsetof(Unit, start_limit.interval), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("StartLimitIntervalSec", "t", bus_property_get_usec, offsetof(Unit, start_limit.interval), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_VTABLE_END
};

static int property_get_slice(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(u);

        return sd_bus_message_append(reply, "s", unit_slice_name(u));
}

static int property_get_current_memory(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        uint64_t sz = (uint64_t) -1;
        Unit *u = userdata;
        int r;

        assert(bus);
        assert(reply);
        assert(u);

        r = unit_get_memory_current(u, &sz);
        if (r < 0 && r != -ENODATA)
                log_unit_warning_errno(u, r, "Failed to get memory.usage_in_bytes attribute: %m");

        return sd_bus_message_append(reply, "t", sz);
}

static int property_get_current_tasks(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        uint64_t cn = (uint64_t) -1;
        Unit *u = userdata;
        int r;

        assert(bus);
        assert(reply);
        assert(u);

        r = unit_get_tasks_current(u, &cn);
        if (r < 0 && r != -ENODATA)
                log_unit_warning_errno(u, r, "Failed to get pids.current attribute: %m");

        return sd_bus_message_append(reply, "t", cn);
}

static int property_get_cpu_usage(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        nsec_t ns = (nsec_t) -1;
        Unit *u = userdata;
        int r;

        assert(bus);
        assert(reply);
        assert(u);

        r = unit_get_cpu_usage(u, &ns);
        if (r < 0 && r != -ENODATA)
                log_unit_warning_errno(u, r, "Failed to get cpuacct.usage attribute: %m");

        return sd_bus_message_append(reply, "t", ns);
}

static int property_get_cgroup(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Unit *u = userdata;
        const char *t = NULL;

        assert(bus);
        assert(reply);
        assert(u);

        /* Three cases: a) u->cgroup_path is NULL, in which case the
         * unit has no control group, which we report as the empty
         * string. b) u->cgroup_path is the empty string, which
         * indicates the root cgroup, which we report as "/". c) all
         * other cases we report as-is. */

        if (u->cgroup_path)
                t = empty_to_root(u->cgroup_path);

        return sd_bus_message_append(reply, "s", t);
}

static int append_process(sd_bus_message *reply, const char *p, pid_t pid, Set *pids) {
        _cleanup_free_ char *buf = NULL, *cmdline = NULL;
        int r;

        assert(reply);
        assert(pid > 0);

        r = set_put(pids, PID_TO_PTR(pid));
        if (IN_SET(r, 0, -EEXIST))
                return 0;
        if (r < 0)
                return r;

        if (!p) {
                r = cg_pid_get_path(SYSTEMD_CGROUP_CONTROLLER, pid, &buf);
                if (r == -ESRCH)
                        return 0;
                if (r < 0)
                        return r;

                p = buf;
        }

        (void) get_process_cmdline(pid, 0, true, &cmdline);

        return sd_bus_message_append(reply,
                                     "(sus)",
                                     p,
                                     (uint32_t) pid,
                                     cmdline);
}

static int append_cgroup(sd_bus_message *reply, const char *p, Set *pids) {
        _cleanup_closedir_ DIR *d = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(reply);
        assert(p);

        r = cg_enumerate_processes(SYSTEMD_CGROUP_CONTROLLER, p, &f);
        if (r == ENOENT)
                return 0;
        if (r < 0)
                return r;

        for (;;) {
                pid_t pid;

                r = cg_read_pid(f, &pid);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (is_kernel_thread(pid) > 0)
                        continue;

                r = append_process(reply, p, pid, pids);
                if (r < 0)
                        return r;
        }

        r = cg_enumerate_subgroups(SYSTEMD_CGROUP_CONTROLLER, p, &d);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return r;

        for (;;) {
                _cleanup_free_ char *g = NULL, *j = NULL;

                r = cg_read_subgroup(d, &g);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                j = strjoin(p, "/", g);
                if (!j)
                        return -ENOMEM;

                r = append_cgroup(reply, j, pids);
                if (r < 0)
                        return r;
        }

        return 0;
}

int bus_unit_method_get_processes(sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(set_freep) Set *pids = NULL;
        Unit *u = userdata;
        pid_t pid;
        int r;

        assert(message);

        r = mac_selinux_unit_access_check(u, message, "status", error);
        if (r < 0)
                return r;

        pids = set_new(NULL);
        if (!pids)
                return -ENOMEM;

        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "(sus)");
        if (r < 0)
                return r;

        if (u->cgroup_path) {
                r = append_cgroup(reply, u->cgroup_path, pids);
                if (r < 0)
                        return r;
        }

        /* The main and control pids might live outside of the cgroup, hence fetch them separately */
        pid = unit_main_pid(u);
        if (pid > 0) {
                r = append_process(reply, NULL, pid, pids);
                if (r < 0)
                        return r;
        }

        pid = unit_control_pid(u);
        if (pid > 0) {
                r = append_process(reply, NULL, pid, pids);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(NULL, reply, NULL);
}

static int property_get_ip_counter(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        CGroupIPAccountingMetric metric;
        uint64_t value = (uint64_t) -1;
        Unit *u = userdata;

        assert(bus);
        assert(reply);
        assert(property);
        assert(u);

        if (streq(property, "IPIngressBytes"))
                metric = CGROUP_IP_INGRESS_BYTES;
        else if (streq(property, "IPIngressPackets"))
                metric = CGROUP_IP_INGRESS_PACKETS;
        else if (streq(property, "IPEgressBytes"))
                metric = CGROUP_IP_EGRESS_BYTES;
        else {
                assert(streq(property, "IPEgressPackets"));
                metric = CGROUP_IP_EGRESS_PACKETS;
        }

        (void) unit_get_ip_accounting(u, metric, &value);
        return sd_bus_message_append(reply, "t", value);
}

int bus_unit_method_attach_processes(sd_bus_message *message, void *userdata, sd_bus_error *error) {

        _cleanup_(sd_bus_creds_unrefp) sd_bus_creds *creds = NULL;
        _cleanup_(set_freep) Set *pids = NULL;
        Unit *u = userdata;
        const char *path;
        int r;

        assert(message);

        /* This migrates the processes with the specified PIDs into the cgroup of this unit, optionally below a
         * specified cgroup path. Obviously this only works for units that actually maintain a cgroup
         * representation. If a process is already in the cgroup no operation is executed – in this case the specified
         * subcgroup path has no effect! */

        r = mac_selinux_unit_access_check(u, message, "start", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "s", &path);
        if (r < 0)
                return r;

        path = empty_to_null(path);
        if (path) {
                if (!path_is_absolute(path))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Control group path is not absolute: %s", path);

                if (!path_is_normalized(path))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Control group path is not normalized: %s", path);
        }

        if (!unit_cgroup_delegate(u))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Process migration not available on non-delegated units.");

        if (UNIT_IS_INACTIVE_OR_FAILED(unit_active_state(u)))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unit is not active, refusing.");

        r = sd_bus_query_sender_creds(message, SD_BUS_CREDS_EUID|SD_BUS_CREDS_PID, &creds);
        if (r < 0)
                return r;

        r = sd_bus_message_enter_container(message, 'a', "u");
        if (r < 0)
                return r;
        for (;;) {
                uid_t process_uid, sender_uid;
                uint32_t upid;
                pid_t pid;

                r = sd_bus_message_read(message, "u", &upid);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (upid == 0) {
                        r = sd_bus_creds_get_pid(creds, &pid);
                        if (r < 0)
                                return r;
                } else
                        pid = (uid_t) upid;

                /* Filter out duplicates */
                if (set_contains(pids, PID_TO_PTR(pid)))
                        continue;

                /* Check if this process is suitable for attaching to this unit */
                r = unit_pid_attachable(u, pid, error);
                if (r < 0)
                        return r;

                /* Let's query the sender's UID, so that we can make our security decisions */
                r = sd_bus_creds_get_euid(creds, &sender_uid);
                if (r < 0)
                        return r;

                /* Let's validate security: if the sender is root, then all is OK. If the sender is is any other unit,
                 * then the process' UID and the target unit's UID have to match the sender's UID */
                if (sender_uid != 0 && sender_uid != getuid()) {
                        r = get_process_uid(pid, &process_uid);
                        if (r < 0)
                                return sd_bus_error_set_errnof(error, r, "Failed to retrieve process UID: %m");

                        if (process_uid != sender_uid)
                                return sd_bus_error_setf(error, SD_BUS_ERROR_ACCESS_DENIED, "Process " PID_FMT " not owned by client's UID. Refusing.", pid);
                        if (process_uid != u->ref_uid)
                                return sd_bus_error_setf(error, SD_BUS_ERROR_ACCESS_DENIED, "Process " PID_FMT " not owned by target unit's UID. Refusing.", pid);
                }

                if (!pids) {
                        pids = set_new(NULL);
                        if (!pids)
                                return -ENOMEM;
                }

                r = set_put(pids, PID_TO_PTR(pid));
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(message);
        if (r < 0)
                return r;

        r = unit_attach_pids_to_cgroup(u, pids, path);
        if (r < 0)
                return sd_bus_error_set_errnof(error, r, "Failed to attach processes to control group: %m");

        return sd_bus_reply_method_return(message, NULL);
}

const sd_bus_vtable bus_unit_cgroup_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Slice", "s", property_get_slice, 0, 0),
        SD_BUS_PROPERTY("ControlGroup", "s", property_get_cgroup, 0, 0),
        SD_BUS_PROPERTY("MemoryCurrent", "t", property_get_current_memory, 0, 0),
        SD_BUS_PROPERTY("CPUUsageNSec", "t", property_get_cpu_usage, 0, 0),
        SD_BUS_PROPERTY("TasksCurrent", "t", property_get_current_tasks, 0, 0),
        SD_BUS_PROPERTY("IPIngressBytes", "t", property_get_ip_counter, 0, 0),
        SD_BUS_PROPERTY("IPIngressPackets", "t", property_get_ip_counter, 0, 0),
        SD_BUS_PROPERTY("IPEgressBytes", "t", property_get_ip_counter, 0, 0),
        SD_BUS_PROPERTY("IPEgressPackets", "t", property_get_ip_counter, 0, 0),
        SD_BUS_METHOD("GetProcesses", NULL, "a(sus)", bus_unit_method_get_processes, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("AttachProcesses", "sau", NULL, bus_unit_method_attach_processes, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END
};

static int send_new_signal(sd_bus *bus, void *userdata) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        _cleanup_free_ char *p = NULL;
        Unit *u = userdata;
        int r;

        assert(bus);
        assert(u);

        p = unit_dbus_path(u);
        if (!p)
                return -ENOMEM;

        r = sd_bus_message_new_signal(
                        bus,
                        &m,
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "UnitNew");
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "so", u->id, p);
        if (r < 0)
                return r;

        return sd_bus_send(bus, m, NULL);
}

static int send_changed_signal(sd_bus *bus, void *userdata) {
        _cleanup_free_ char *p = NULL;
        Unit *u = userdata;
        int r;

        assert(bus);
        assert(u);

        p = unit_dbus_path(u);
        if (!p)
                return -ENOMEM;

        /* Send a properties changed signal. First for the specific
         * type, then for the generic unit. The clients may rely on
         * this order to get atomic behavior if needed. */

        r = sd_bus_emit_properties_changed_strv(
                        bus, p,
                        unit_dbus_interface_from_type(u->type),
                        NULL);
        if (r < 0)
                return r;

        return sd_bus_emit_properties_changed_strv(
                        bus, p,
                        "org.freedesktop.systemd1.Unit",
                        NULL);
}

void bus_unit_send_change_signal(Unit *u) {
        int r;
        assert(u);

        if (u->in_dbus_queue) {
                LIST_REMOVE(dbus_queue, u->manager->dbus_unit_queue, u);
                u->in_dbus_queue = false;
        }

        if (!u->id)
                return;

        r = bus_foreach_bus(u->manager, u->bus_track, u->sent_dbus_new_signal ? send_changed_signal : send_new_signal, u);
        if (r < 0)
                log_unit_debug_errno(u, r, "Failed to send unit change signal for %s: %m", u->id);

        u->sent_dbus_new_signal = true;
}

static int send_removed_signal(sd_bus *bus, void *userdata) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        _cleanup_free_ char *p = NULL;
        Unit *u = userdata;
        int r;

        assert(bus);
        assert(u);

        p = unit_dbus_path(u);
        if (!p)
                return -ENOMEM;

        r = sd_bus_message_new_signal(
                        bus,
                        &m,
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "UnitRemoved");
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "so", u->id, p);
        if (r < 0)
                return r;

        return sd_bus_send(bus, m, NULL);
}

void bus_unit_send_removed_signal(Unit *u) {
        int r;
        assert(u);

        if (!u->sent_dbus_new_signal || u->in_dbus_queue)
                bus_unit_send_change_signal(u);

        if (!u->id)
                return;

        r = bus_foreach_bus(u->manager, u->bus_track, send_removed_signal, u);
        if (r < 0)
                log_unit_debug_errno(u, r, "Failed to send unit remove signal for %s: %m", u->id);
}

int bus_unit_queue_job(
                sd_bus_message *message,
                Unit *u,
                JobType type,
                JobMode mode,
                bool reload_if_possible,
                sd_bus_error *error) {

        _cleanup_free_ char *path = NULL;
        Job *j;
        int r;

        assert(message);
        assert(u);
        assert(type >= 0 && type < _JOB_TYPE_MAX);
        assert(mode >= 0 && mode < _JOB_MODE_MAX);

        r = mac_selinux_unit_access_check(
                        u, message,
                        job_type_to_access_method(type),
                        error);
        if (r < 0)
                return r;

        if (reload_if_possible && unit_can_reload(u)) {
                if (type == JOB_RESTART)
                        type = JOB_RELOAD_OR_START;
                else if (type == JOB_TRY_RESTART)
                        type = JOB_TRY_RELOAD;
        }

        if (type == JOB_STOP &&
            (IN_SET(u->load_state, UNIT_NOT_FOUND, UNIT_ERROR)) &&
            unit_active_state(u) == UNIT_INACTIVE)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_UNIT, "Unit %s not loaded.", u->id);

        if ((type == JOB_START && u->refuse_manual_start) ||
            (type == JOB_STOP && u->refuse_manual_stop) ||
            (IN_SET(type, JOB_RESTART, JOB_TRY_RESTART) && (u->refuse_manual_start || u->refuse_manual_stop)) ||
            (type == JOB_RELOAD_OR_START && job_type_collapse(type, u) == JOB_START && u->refuse_manual_start))
                return sd_bus_error_setf(error, BUS_ERROR_ONLY_BY_DEPENDENCY, "Operation refused, unit %s may be requested by dependency only (it is configured to refuse manual start/stop).", u->id);

        r = manager_add_job(u->manager, type, u, mode, error, &j);
        if (r < 0)
                return r;

        r = bus_job_track_sender(j, message);
        if (r < 0)
                return r;

        path = job_dbus_path(j);
        if (!path)
                return -ENOMEM;

        return sd_bus_reply_method_return(message, "o", path);
}

static int bus_unit_set_live_property(
                Unit *u,
                const char *name,
                sd_bus_message *message,
                UnitWriteFlags flags,
                sd_bus_error *error) {

        int r;

        assert(u);
        assert(name);
        assert(message);

        /* Handles setting properties both "live" (i.e. at any time during runtime), and during creation (for transient
         * units that are being created). */

        if (streq(name, "Description")) {
                const char *d;

                r = sd_bus_message_read(message, "s", &d);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        r = unit_set_description(u, d);
                        if (r < 0)
                                return r;

                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "Description=%s", d);
                }

                return 1;
        }

        return 0;
}

static BUS_DEFINE_SET_TRANSIENT_PARSE(collect_mode, CollectMode, collect_mode_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(emergency_action, EmergencyAction, emergency_action_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(job_mode, JobMode, job_mode_from_string);

static int bus_set_transient_conditions(
                Unit *u,
                const char *name,
                Condition **list,
                bool is_condition,
                sd_bus_message *message,
                UnitWriteFlags flags,
                sd_bus_error *error) {

        const char *type_name, *param;
        int trigger, negate, r;
        bool empty = true;

        assert(list);

        r = sd_bus_message_enter_container(message, 'a', "(sbbs)");
        if (r < 0)
                return r;

        while ((r = sd_bus_message_read(message, "(sbbs)", &type_name, &trigger, &negate, &param)) > 0) {
                ConditionType t;

                t = is_condition ? condition_type_from_string(type_name) : assert_type_from_string(type_name);
                if (t < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid condition type: %s", type_name);

                if (t != CONDITION_NULL) {
                        if (isempty(param))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Condition parameter in %s is empty", type_name);

                        if (condition_takes_path(t) && !path_is_absolute(param))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Path in condition %s is not absolute: %s", type_name, param);
                } else
                        param = NULL;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        Condition *c;

                        c = condition_new(t, param, trigger, negate);
                        if (!c)
                                return -ENOMEM;

                        LIST_PREPEND(conditions, *list, c);

                        if (t != CONDITION_NULL)
                                unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name,
                                                    "%s=%s%s%s", type_name,
                                                    trigger ? "|" : "", negate ? "!" : "", param);
                        else
                                unit_write_settingf(u, flags, name,
                                                    "%s=%s%s", type_name,
                                                    trigger ? "|" : "", yes_no(!negate));
                }

                empty = false;
        }
        if (r < 0)
                return r;

        r = sd_bus_message_exit_container(message);
        if (r < 0)
                return r;

        if (!UNIT_WRITE_FLAGS_NOOP(flags) && empty) {
                *list = condition_free_list(*list);
                unit_write_settingf(u, flags, name, "%sNull=", is_condition ? "Condition" : "Assert");
        }

        return 1;
}

static int bus_unit_set_transient_property(
                Unit *u,
                const char *name,
                sd_bus_message *message,
                UnitWriteFlags flags,
                sd_bus_error *error) {

        UnitDependency d = _UNIT_DEPENDENCY_INVALID;
        int r;

        assert(u);
        assert(name);
        assert(message);

        /* Handles settings when transient units are created. This settings cannot be altered anymore after the unit
         * has been created. */

        if (streq(name, "SourcePath"))
                return bus_set_transient_path(u, name, &u->source_path, message, flags, error);

        if (streq(name, "StopWhenUnneeded"))
                return bus_set_transient_bool(u, name, &u->stop_when_unneeded, message, flags, error);

        if (streq(name, "RefuseManualStart"))
                return bus_set_transient_bool(u, name, &u->refuse_manual_start, message, flags, error);

        if (streq(name, "RefuseManualStop"))
                return bus_set_transient_bool(u, name, &u->refuse_manual_stop, message, flags, error);

        if (streq(name, "AllowIsolate"))
                return bus_set_transient_bool(u, name, &u->allow_isolate, message, flags, error);

        if (streq(name, "DefaultDependencies"))
                return bus_set_transient_bool(u, name, &u->default_dependencies, message, flags, error);

        if (streq(name, "OnFailureJobMode"))
                return bus_set_transient_job_mode(u, name, &u->on_failure_job_mode, message, flags, error);

        if (streq(name, "IgnoreOnIsolate"))
                return bus_set_transient_bool(u, name, &u->ignore_on_isolate, message, flags, error);

        if (streq(name, "JobTimeoutUSec")) {
                r = bus_set_transient_usec_fix_0(u, name, &u->job_timeout, message, flags, error);
                if (r >= 0 && !UNIT_WRITE_FLAGS_NOOP(flags) && !u->job_running_timeout_set)
                        u->job_running_timeout = u->job_timeout;
        }

        if (streq(name, "JobRunningTimeoutUSec")) {
                r = bus_set_transient_usec_fix_0(u, name, &u->job_running_timeout, message, flags, error);
                if (r >= 0 && !UNIT_WRITE_FLAGS_NOOP(flags))
                        u->job_running_timeout_set = true;

                return r;
        }

        if (streq(name, "JobTimeoutAction"))
                return bus_set_transient_emergency_action(u, name, &u->job_timeout_action, message, flags, error);

        if (streq(name, "JobTimeoutRebootArgument"))
                return bus_set_transient_string(u, name, &u->job_timeout_reboot_arg, message, flags, error);

        if (streq(name, "StartLimitIntervalUSec"))
                return bus_set_transient_usec(u, name, &u->start_limit.interval, message, flags, error);

        if (streq(name, "StartLimitBurst"))
                return bus_set_transient_unsigned(u, name, &u->start_limit.burst, message, flags, error);

        if (streq(name, "StartLimitAction"))
                return bus_set_transient_emergency_action(u, name, &u->start_limit_action, message, flags, error);

        if (streq(name, "FailureAction"))
                return bus_set_transient_emergency_action(u, name, &u->failure_action, message, flags, error);

        if (streq(name, "SuccessAction"))
                return bus_set_transient_emergency_action(u, name, &u->success_action, message, flags, error);

        if (streq(name, "RebootArgument"))
                return bus_set_transient_string(u, name, &u->reboot_arg, message, flags, error);

        if (streq(name, "CollectMode"))
                return bus_set_transient_collect_mode(u, name, &u->collect_mode, message, flags, error);

        if (streq(name, "Conditions"))
                return bus_set_transient_conditions(u, name, &u->conditions, true, message, flags, error);

        if (streq(name, "Asserts"))
                return bus_set_transient_conditions(u, name, &u->asserts, false, message, flags, error);

        if (streq(name, "Documentation")) {
                _cleanup_strv_free_ char **l = NULL;
                char **p;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l) {
                        if (!documentation_url_is_valid(*p))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid URL in %s: %s", name, *p);
                }

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (strv_isempty(l)) {
                                u->documentation = strv_free(u->documentation);
                                unit_write_settingf(u, flags, name, "%s=", name);
                        } else {
                                strv_extend_strv(&u->documentation, l, false);

                                STRV_FOREACH(p, l)
                                        unit_write_settingf(u, flags, name, "%s=%s", name, *p);
                        }
                }

                return 1;

        } else if (streq(name, "Slice")) {
                Unit *slice;
                const char *s;

                if (!UNIT_HAS_CGROUP_CONTEXT(u))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "The slice property is only available for units with control groups.");
                if (u->type == UNIT_SLICE)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Slice may not be set for slice units.");
                if (unit_has_name(u, SPECIAL_INIT_SCOPE))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Cannot set slice for init.scope");

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                if (!unit_name_is_valid(s, UNIT_NAME_PLAIN))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid unit name '%s'", s);

                /* Note that we do not dispatch the load queue here yet, as we don't want our own transient unit to be
                 * loaded while we are still setting it up. Or in other words, we use manager_load_unit_prepare()
                 * instead of manager_load_unit() on purpose, here. */
                r = manager_load_unit_prepare(u->manager, s, NULL, error, &slice);
                if (r < 0)
                        return r;

                if (slice->type != UNIT_SLICE)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unit name '%s' is not a slice", s);

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        r = unit_set_slice(u, slice);
                        if (r < 0)
                                return r;

                        unit_write_settingf(u, flags|UNIT_PRIVATE, name, "Slice=%s", s);
                }

                return 1;

        } else if (streq(name, "RequiresMountsFor")) {
                _cleanup_strv_free_ char **l = NULL;
                char **p;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l) {
                        if (!path_is_absolute(*p))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Path specified in %s is not absolute: %s", name, *p);

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                r = unit_require_mounts_for(u, *p, UNIT_DEPENDENCY_FILE);
                                if (r < 0)
                                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Failed to add required mount \"%s\": %m", *p);

                                unit_write_settingf(u, flags, name, "%s=%s", name, *p);
                        }
                }

                return 1;
        }

        if (streq(name, "RequiresOverridable"))
                d = UNIT_REQUIRES; /* redirect for obsolete unit dependency type */
        else if (streq(name, "RequisiteOverridable"))
                d = UNIT_REQUISITE; /* same here */
        else
                d = unit_dependency_from_string(name);

        if (d >= 0) {
                const char *other;

                r = sd_bus_message_enter_container(message, 'a', "s");
                if (r < 0)
                        return r;

                while ((r = sd_bus_message_read(message, "s", &other)) > 0) {
                        if (!unit_name_is_valid(other, UNIT_NAME_PLAIN|UNIT_NAME_INSTANCE))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid unit name %s", other);

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                _cleanup_free_ char *label = NULL;

                                r = unit_add_dependency_by_name(u, d, other, NULL, true, UNIT_DEPENDENCY_FILE);
                                if (r < 0)
                                        return r;

                                label = strjoin(name, "-", other);
                                if (!label)
                                        return -ENOMEM;

                                unit_write_settingf(u, flags, label, "%s=%s", unit_dependency_to_string(d), other);
                        }

                }
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                return 1;

        } else if (streq(name, "AddRef")) {

                int b;

                /* Why is this called "AddRef" rather than just "Ref", or "Reference"? There's already a "Ref()" method
                 * on the Unit interface, and it's probably not a good idea to expose a property and a method on the
                 * same interface (well, strictly speaking AddRef isn't exposed as full property, we just read it for
                 * transient units, but still). And "References" and "ReferencedBy" is already used as unit reference
                 * dependency type, hence let's not confuse things with that.
                 *
                 * Note that we don't acually add the reference to the bus track. We do that only after the setup of
                 * the transient unit is complete, so that setting this property multiple times in the same transient
                 * unit creation call doesn't count as individual references. */

                r = sd_bus_message_read(message, "b", &b);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags))
                        u->bus_track_add = b;

                return 1;
        }

        return 0;
}

int bus_unit_set_properties(
                Unit *u,
                sd_bus_message *message,
                UnitWriteFlags flags,
                bool commit,
                sd_bus_error *error) {

        bool for_real = false;
        unsigned n = 0;
        int r;

        assert(u);
        assert(message);

        /* We iterate through the array twice. First run we just check
         * if all passed data is valid, second run actually applies
         * it. This is to implement transaction-like behaviour without
         * actually providing full transactions. */

        r = sd_bus_message_enter_container(message, 'a', "(sv)");
        if (r < 0)
                return r;

        for (;;) {
                const char *name;
                UnitWriteFlags f;

                r = sd_bus_message_enter_container(message, 'r', "sv");
                if (r < 0)
                        return r;
                if (r == 0) {
                        if (for_real || UNIT_WRITE_FLAGS_NOOP(flags))
                                break;

                        /* Reached EOF. Let's try again, and this time for realz... */
                        r = sd_bus_message_rewind(message, false);
                        if (r < 0)
                                return r;

                        for_real = true;
                        continue;
                }

                r = sd_bus_message_read(message, "s", &name);
                if (r < 0)
                        return r;

                if (!UNIT_VTABLE(u)->bus_set_property)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_PROPERTY_READ_ONLY, "Objects of this type do not support setting properties.");

                r = sd_bus_message_enter_container(message, 'v', NULL);
                if (r < 0)
                        return r;

                /* If not for real, then mask out the two target flags */
                f = for_real ? flags : (flags & ~(UNIT_RUNTIME|UNIT_PERSISTENT));

                r = UNIT_VTABLE(u)->bus_set_property(u, name, message, f, error);
                if (r == 0 && u->transient && u->load_state == UNIT_STUB)
                        r = bus_unit_set_transient_property(u, name, message, f, error);
                if (r == 0)
                        r = bus_unit_set_live_property(u, name, message, f, error);
                if (r < 0)
                        return r;

                if (r == 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_PROPERTY_READ_ONLY, "Cannot set property %s, or unknown property.", name);

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                n += for_real;
        }

        r = sd_bus_message_exit_container(message);
        if (r < 0)
                return r;

        if (commit && n > 0 && UNIT_VTABLE(u)->bus_commit_properties)
                UNIT_VTABLE(u)->bus_commit_properties(u);

        return n;
}

int bus_unit_check_load_state(Unit *u, sd_bus_error *error) {
        assert(u);

        if (u->load_state == UNIT_LOADED)
                return 0;

        /* Give a better description of the unit error when
         * possible. Note that in the case of UNIT_MASKED, load_error
         * is not set. */
        if (u->load_state == UNIT_MASKED)
                return sd_bus_error_setf(error, BUS_ERROR_UNIT_MASKED, "Unit %s is masked.", u->id);

        if (u->load_state == UNIT_NOT_FOUND)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_UNIT, "Unit %s not found.", u->id);

        return sd_bus_error_set_errnof(error, u->load_error, "Unit %s is not loaded properly: %m.", u->id);
}

static int bus_unit_track_handler(sd_bus_track *t, void *userdata) {
        Unit *u = userdata;

        assert(t);
        assert(u);

        u->bus_track = sd_bus_track_unref(u->bus_track); /* make sure we aren't called again */

        unit_add_to_gc_queue(u);
        return 0;
}

static int bus_unit_allocate_bus_track(Unit *u) {
        int r;

        assert(u);

        if (u->bus_track)
                return 0;

        r = sd_bus_track_new(u->manager->api_bus, &u->bus_track, bus_unit_track_handler, u);
        if (r < 0)
                return r;

        r = sd_bus_track_set_recursive(u->bus_track, true);
        if (r < 0) {
                u->bus_track = sd_bus_track_unref(u->bus_track);
                return r;
        }

        return 0;
}

int bus_unit_track_add_name(Unit *u, const char *name) {
        int r;

        assert(u);

        r = bus_unit_allocate_bus_track(u);
        if (r < 0)
                return r;

        return sd_bus_track_add_name(u->bus_track, name);
}

int bus_unit_track_add_sender(Unit *u, sd_bus_message *m) {
        int r;

        assert(u);

        r = bus_unit_allocate_bus_track(u);
        if (r < 0)
                return r;

        return sd_bus_track_add_sender(u->bus_track, m);
}

int bus_unit_track_remove_sender(Unit *u, sd_bus_message *m) {
        assert(u);

        /* If we haven't allocated the bus track object yet, then there's definitely no reference taken yet, return an
         * error */
        if (!u->bus_track)
                return -EUNATCH;

        return sd_bus_track_remove_sender(u->bus_track, m);
}
