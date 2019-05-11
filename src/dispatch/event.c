#include "event.h"

extern struct eventloop g_eventloop;
extern struct display_manager g_display_manager;
extern struct space_manager g_space_manager;
extern struct window_manager g_window_manager;
extern int g_connection;

static struct ax_window *mouse_window;
static CGRect mouse_window_frame;
static bool mouse_left_down;
static uint64_t mouse_moved_time;

static EVENT_CALLBACK(EVENT_HANDLER_APPLICATION_LAUNCHED)
{
    struct process *process = context;

    if ((process->terminated) || (kill(process->pid, 0) == -1)) {
        window_manager_remove_lost_activated_event(&g_window_manager, process->pid);
        return;
    }

    struct ax_application *application = application_create(process);
    if (application_observe(application)) {
        debug("%s: %s\n", __FUNCTION__, process->name);
        window_manager_add_application(&g_window_manager, application);
        window_manager_add_application_windows(&g_window_manager, application);

        int window_count = 0;
        struct ax_window **window_list = window_manager_find_application_windows(&g_window_manager, application, &window_count);
        if (window_list) {
            for (int i = 0; i < window_count; ++i) {
                struct ax_window *window = window_list[i];
                if (!window) continue;

                if (window_manager_should_manage_window(window)) {
                    struct view *view = space_manager_tile_window_on_space(&g_space_manager, window, space_manager_active_space());
                    window_manager_add_managed_window(&g_window_manager, window, view);
                }
            }
            free(window_list);
        }

        if (window_manager_find_lost_activated_event(&g_window_manager, application->pid)) {
            struct event *event;
            event_create(event, APPLICATION_ACTIVATED, (void *)(intptr_t) application->pid);
            eventloop_post(&g_eventloop, event);
            window_manager_remove_lost_activated_event(&g_window_manager, application->pid);
        }
    } else {
        debug("%s: could not observe %s\n", __FUNCTION__, process->name);
        application_unobserve(application);
        application_destroy(application);

        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 0.01f * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            struct event *event;
            event_create(event, APPLICATION_LAUNCHED, process);
            eventloop_post(&g_eventloop, event);
        });
    }
}

static EVENT_CALLBACK(EVENT_HANDLER_APPLICATION_TERMINATED)
{
    struct process *process = context;
    struct ax_application *application = window_manager_find_application(&g_window_manager, process->pid);

    if (application) {
        debug("%s: %s\n", __FUNCTION__, process->name);
        window_manager_remove_application(&g_window_manager, application->pid);

        int window_count = 0;
        struct ax_window **window_list = window_manager_find_application_windows(&g_window_manager, application, &window_count);
        if (window_list) {
            for (int i = 0; i < window_count; ++i) {
                struct ax_window *window = window_list[i];
                if (!window) continue;

                struct view *view = window_manager_find_managed_window(&g_window_manager, window);
                if (view) {
                    space_manager_untile_window(&g_space_manager, view, window);
                    window_manager_remove_managed_window(&g_window_manager, window);
                }

                if (mouse_window == window) mouse_window = NULL;

                window_manager_remove_window(&g_window_manager, window->id);
                window_destroy(window);
            }
            free(window_list);
        }

        application_unobserve(application);
        application_destroy(application);
    } else {
        debug("%s: %s (not observed)\n", __FUNCTION__, process->name);
    }

    process_destroy(process);
}

static EVENT_CALLBACK(EVENT_HANDLER_APPLICATION_ACTIVATED)
{
    struct ax_application *application = window_manager_find_application(&g_window_manager, (pid_t)(intptr_t) context);
    if (!application) {
        window_manager_add_lost_activated_event(&g_window_manager, (pid_t)(intptr_t) context, APPLICATION_ACTIVATED);
        return;
    }

    debug("%s: %s\n", __FUNCTION__, application->name);
    uint32_t application_focused_window_id = application_focused_window(application);
    if (g_window_manager.focused_window_id != application_focused_window_id) {
        g_window_manager.focused_window_id = application_focused_window(application);
        g_window_manager.focused_window_pid = application->pid;

        struct ax_window *focused_window = window_manager_find_window(&g_window_manager, g_window_manager.focused_window_id);
        if (focused_window) {
            border_window_activate(focused_window);
            window_manager_set_window_opacity(focused_window, g_window_manager.active_window_opacity);
            window_manager_center_mouse(&g_window_manager, focused_window);
            g_window_manager.reactivate_focused_window = display_manager_active_display_is_animating();
        }
    }
}

static EVENT_CALLBACK(EVENT_HANDLER_APPLICATION_DEACTIVATED)
{
    struct ax_application *application = window_manager_find_application(&g_window_manager, (pid_t)(intptr_t) context);
    if (!application) return;

    debug("%s: %s\n", __FUNCTION__, application->name);
    struct ax_window *focused_window = window_manager_find_window(&g_window_manager, application_focused_window(application));
    if (focused_window) {
        border_window_deactivate(focused_window);
        window_manager_set_window_opacity(focused_window, g_window_manager.normal_window_opacity);

        if (!window_is_standard(focused_window)) {
            struct ax_window *main_window = window_manager_find_window(&g_window_manager, application_main_window(application));
            if (main_window && main_window != focused_window) {
                border_window_deactivate(main_window);
                window_manager_set_window_opacity(main_window, g_window_manager.normal_window_opacity);
            }
        }
    }
}

static EVENT_CALLBACK(EVENT_HANDLER_APPLICATION_VISIBLE)
{
    struct ax_application *application = window_manager_find_application(&g_window_manager, (pid_t)(intptr_t) context);
    if (!application) return;

    debug("%s: %s\n", __FUNCTION__, application->name);
    application->is_hidden = false;

    int window_count = 0;
    struct ax_window **window_list = window_manager_find_application_windows(&g_window_manager, application, &window_count);
    if (!window_count) return;

    for (int i = 0; i < window_count; ++i) {
        struct ax_window *window = window_list[i];
        if (!window) continue;

        if (window_manager_should_manage_window(window)) {
            int space_count = 0;
            uint64_t *space_list = window_space_list(window, &space_count);
            struct view *view = space_manager_tile_window_on_space(&g_space_manager, window, *space_list);
            window_manager_add_managed_window(&g_window_manager, window, view);
            free(space_list);

            border_window_show(window);
        }
    }

    free(window_list);
}

static EVENT_CALLBACK(EVENT_HANDLER_APPLICATION_HIDDEN)
{
    struct ax_application *application = window_manager_find_application(&g_window_manager, (pid_t)(intptr_t) context);
    if (!application) return;

    debug("%s: %s\n", __FUNCTION__, application->name);
    application->is_hidden = true;

    int window_count = 0;
    struct ax_window **window_list = window_manager_find_application_windows(&g_window_manager, application, &window_count);
    if (!window_count) return;

    for (int i = 0; i < window_count; ++i) {
        struct ax_window *window = window_list[i];
        if (!window) continue;

        border_window_hide(window);

        struct view *view = window_manager_find_managed_window(&g_window_manager, window);
        if (view) {
            space_manager_untile_window(&g_space_manager, view, window);
            window_manager_remove_managed_window(&g_window_manager, window);
        }
    }

    free(window_list);
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_CREATED)
{
    uint32_t window_id = ax_window_id(context);
    if (!window_id) return;

    pid_t window_pid = ax_window_pid(context);
    if (!window_pid) return;

    if (window_manager_find_window(&g_window_manager, window_id)) return;

    struct ax_application *application = window_manager_find_application(&g_window_manager, window_pid);
    if (!application) return;

    struct ax_window *window = window_create(application, context);
    window_manager_set_window_opacity(window, g_window_manager.normal_window_opacity);

    if (window_observe(window)) {
        debug("%s: %s %d\n", __FUNCTION__, window->application->name, window->id);
        window_manager_add_window(&g_window_manager, window);

        if ((!window_is_standard(window)) ||
            (!window_can_move(window)) ||
            (!window_can_resize(window))) {
            window->is_floating = true;
        }

        if (window_manager_should_manage_window(window)) {
            struct view *view = space_manager_tile_window_on_space(&g_space_manager, window, space_manager_active_space());
            window_manager_add_managed_window(&g_window_manager, window, view);
        }

        if (window_manager_find_lost_focused_event(&g_window_manager, window->id)) {
            struct event *event;
            event_create(event, WINDOW_FOCUSED, (void *)(intptr_t) window->id);
            eventloop_post(&g_eventloop, event);
            window_manager_remove_lost_focused_event(&g_window_manager, window->id);
        }
    } else {
        debug("%s: could not observe %s %d\n", __FUNCTION__, window->application->name, window->id);
        window_manager_remove_lost_focused_event(&g_window_manager, window->id);
        window_unobserve(window);
        window_destroy(window);
    }
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_SHEET_CREATED)
{
    uint32_t window_id = ax_window_id(context);
    if (!window_id) return;

    debug("%s: %d\n", __FUNCTION__, window_id);
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_DRAWER_CREATED)
{
    uint32_t window_id = ax_window_id(context);
    if (!window_id) return;

    debug("%s: %d\n", __FUNCTION__, window_id);
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_DESTROYED)
{
    uint32_t window_id = (uint32_t)(uintptr_t) context;
    struct ax_window *window = window_manager_find_window(&g_window_manager, window_id);
    if (!window) return;

    debug("%s: %s %d\n", __FUNCTION__, window->application->name, window->id);
    assert(!*window->id_ptr);

    struct view *view = window_manager_find_managed_window(&g_window_manager, window);
    if (view) {
        space_manager_untile_window(&g_space_manager, view, window);
        window_manager_remove_managed_window(&g_window_manager, window);
    }

    if (mouse_window == window) mouse_window = NULL;

    window_manager_remove_window(&g_window_manager, window->id);
    window_unobserve(window);
    window_destroy(window);
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_FOCUSED)
{
    uint32_t window_id = (uint32_t)(intptr_t) context;
    struct ax_window *window = window_manager_find_window(&g_window_manager, window_id);

    if (!window) {
        window_manager_add_lost_focused_event(&g_window_manager, window_id, WINDOW_FOCUSED);
        return;
    }

    if (!__sync_bool_compare_and_swap(window->id_ptr, &window->id, &window->id)) {
        debug("%s: %d has been marked invalid by the system, ignoring event..\n", __FUNCTION__, window_id);
        return;
    }

    if (window_is_minimized(window)) {
        window_manager_add_lost_focused_event(&g_window_manager, window->id, WINDOW_FOCUSED);
        return;
    }

    debug("%s: %s %d\n", __FUNCTION__, window->application->name, window->id);
    struct ax_window *focused_window = window_manager_find_window(&g_window_manager, g_window_manager.focused_window_id);
    if (focused_window) {
        border_window_deactivate(focused_window);
        window_manager_set_window_opacity(focused_window, g_window_manager.normal_window_opacity);
    }

    border_window_activate(window);
    window_manager_set_window_opacity(window, g_window_manager.active_window_opacity);
    window_manager_center_mouse(&g_window_manager, window);

    g_window_manager.focused_window_id = window->id;
    g_window_manager.focused_window_pid = window->application->pid;
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_MOVED)
{
    uint32_t window_id = (uint32_t)(intptr_t) context;
    struct ax_window *window = window_manager_find_window(&g_window_manager, window_id);
    if (!window) return;

    if (!__sync_bool_compare_and_swap(window->id_ptr, &window->id, &window->id)) {
        debug("%s: %d has been marked invalid by the system, ignoring event..\n", __FUNCTION__, window_id);
        return;
    }

    if (window->application->is_hidden) return;

    debug("%s: %s %d\n", __FUNCTION__, window->application->name, window->id);

#if 0
    struct view *view = window_manager_find_managed_window(&g_window_manager, window);
    if (view) view_flush(view);
#endif

    border_window_refresh(window);
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_RESIZED)
{
    uint32_t window_id = (uint32_t)(intptr_t) context;
    struct ax_window *window = window_manager_find_window(&g_window_manager, window_id);
    if (!window) return;

    if (!__sync_bool_compare_and_swap(window->id_ptr, &window->id, &window->id)) {
        debug("%s: %d has been marked invalid by the system, ignoring event..\n", __FUNCTION__, window_id);
        return;
    }

    if (window->application->is_hidden) return;

    debug("%s: %s %d\n", __FUNCTION__, window->application->name, window->id);

#if 0
    struct view *view = window_manager_find_managed_window(&g_window_manager, window);
    if (view) view_flush(view);
#endif

    border_window_refresh(window);
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_MINIMIZED)
{
    uint32_t window_id = (uint32_t)(intptr_t) context;
    struct ax_window *window = window_manager_find_window(&g_window_manager, window_id);
    if (!window) return;

    if (!__sync_bool_compare_and_swap(window->id_ptr, &window->id, &window->id)) {
        debug("%s: %d has been marked invalid by the system, ignoring event..\n", __FUNCTION__, window_id);
        return;
    }

    debug("%s: %s %d\n", __FUNCTION__, window->application->name, window->id);
    window->is_minimized = true;
    border_window_hide(window);

    struct view *view = window_manager_find_managed_window(&g_window_manager, window);
    if (view) {
        space_manager_untile_window(&g_space_manager, view, window);
        window_manager_remove_managed_window(&g_window_manager, window);
    }
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_DEMINIMIZED)
{
    uint32_t window_id = (uint32_t)(intptr_t) context;
    struct ax_window *window = window_manager_find_window(&g_window_manager, window_id);
    if (!window) return;

    if (!__sync_bool_compare_and_swap(window->id_ptr, &window->id, &window->id)) {
        debug("%s: %d has been marked invalid by the system, ignoring event..\n", __FUNCTION__, window_id);
        window_manager_remove_lost_focused_event(&g_window_manager, window_id);
        return;
    }

    window->is_minimized = false;
    border_window_show(window);

    if (space_manager_is_window_on_active_space(window)) {
        debug("%s: window %s %d is deminimized on active space\n", __FUNCTION__, window->application->name, window->id);
        if (window_manager_should_manage_window(window)) {
            struct view *view = space_manager_tile_window_on_space(&g_space_manager, window, space_manager_active_space());
            window_manager_add_managed_window(&g_window_manager, window, view);
        }
    } else {
        debug("%s: window %s %d is deminimized on inactive space\n", __FUNCTION__, window->application->name, window->id);
    }

    if (window_manager_find_lost_focused_event(&g_window_manager, window->id)) {
        struct event *event;
        event_create(event, WINDOW_FOCUSED, (void *)(intptr_t) window->id);
        eventloop_post(&g_eventloop, event);
        window_manager_remove_lost_focused_event(&g_window_manager, window->id);
    }
}

static EVENT_CALLBACK(EVENT_HANDLER_WINDOW_TITLE_CHANGED)
{
    uint32_t window_id = (uint32_t)(intptr_t) context;
    struct ax_window *window = window_manager_find_window(&g_window_manager, window_id);
    if (!window) return;

    if (!__sync_bool_compare_and_swap(window->id_ptr, &window->id, &window->id)) {
        debug("%s: %d has been marked invalid by the system, ignoring event..\n", __FUNCTION__, window_id);
        return;
    }

    debug("%s: %s %d\n", __FUNCTION__, window->application->name, window->id);
}

static EVENT_CALLBACK(EVENT_HANDLER_SPACE_CHANGED)
{
    g_space_manager.last_space_id = g_space_manager.current_space_id;
    g_space_manager.current_space_id = space_manager_active_space();

    debug("%s: %lld\n", __FUNCTION__, g_space_manager.current_space_id);
    struct view *view = space_manager_find_view(&g_space_manager, g_space_manager.current_space_id);

    if (view_is_invalid(view)) view_update(view);
    if (view_is_dirty(view))   view_flush(view);

    if (space_manager_refresh_application_windows() || g_window_manager.reactivate_focused_window) {
        struct ax_window *focused_window = window_manager_find_window(&g_window_manager, g_window_manager.focused_window_id);
        if (focused_window) {
            border_window_activate(focused_window);
            window_manager_set_window_opacity(focused_window, g_window_manager.active_window_opacity);
            window_manager_center_mouse(&g_window_manager, focused_window);
            g_window_manager.reactivate_focused_window = false;
        }
    }

    window_manager_validate_windows_on_space(&g_space_manager, &g_window_manager, g_space_manager.current_space_id);
    window_manager_check_for_windows_on_space(&g_space_manager, &g_window_manager, g_space_manager.current_space_id);
}

static EVENT_CALLBACK(EVENT_HANDLER_DISPLAY_CHANGED)
{
    g_display_manager.last_display_id = g_display_manager.current_display_id;
    g_display_manager.current_display_id = display_manager_active_display_id();

    g_space_manager.last_space_id = g_space_manager.current_space_id;
    g_space_manager.current_space_id = display_space_id(g_display_manager.current_display_id);

    assert(g_display_manager.current_display_id == space_display_id(g_space_manager.current_space_id));
    debug("%s: %d %lld\n", __FUNCTION__, g_display_manager.current_display_id, g_space_manager.current_space_id);
    struct view *view = space_manager_find_view(&g_space_manager, g_space_manager.current_space_id);

    if (view_is_invalid(view)) view_update(view);
    if (view_is_dirty(view))   view_flush(view);

    if (space_manager_refresh_application_windows()) {
        struct ax_window *focused_window = window_manager_find_window(&g_window_manager, g_window_manager.focused_window_id);
        if (focused_window) {
            border_window_activate(focused_window);
            window_manager_center_mouse(&g_window_manager, focused_window);
        }
    }

    window_manager_validate_windows_on_space(&g_space_manager, &g_window_manager, g_space_manager.current_space_id);
    window_manager_check_for_windows_on_space(&g_space_manager, &g_window_manager, g_space_manager.current_space_id);
}

static EVENT_CALLBACK(EVENT_HANDLER_DISPLAY_ADDED)
{
    uint32_t display_id = (uint32_t)(intptr_t) context;
    uint32_t sid = display_space_id(display_id);
    debug("%s: %d\n", __FUNCTION__, display_id);
    window_manager_handle_display_add_and_remove(&g_space_manager, &g_window_manager, display_id, sid);
}

static EVENT_CALLBACK(EVENT_HANDLER_DISPLAY_REMOVED)
{
    uint32_t display_id = display_manager_main_display_id();
    uint32_t sid = display_space_id(display_id);
    debug("%s: %d\n", __FUNCTION__, display_id);
    window_manager_handle_display_add_and_remove(&g_space_manager, &g_window_manager, display_id, sid);
}

static EVENT_CALLBACK(EVENT_HANDLER_DISPLAY_MOVED)
{
    uint32_t display_id = (uint32_t)(intptr_t) context;
    debug("%s: %d\n", __FUNCTION__, display_id);
    space_manager_mark_spaces_invalid(&g_space_manager);
}

static EVENT_CALLBACK(EVENT_HANDLER_DISPLAY_RESIZED)
{
    uint32_t display_id = (uint32_t)(intptr_t) context;
    debug("%s: %d\n", __FUNCTION__, display_id);
    space_manager_mark_spaces_invalid_for_display(&g_space_manager, display_id);
}

static EVENT_CALLBACK(EVENT_HANDLER_MOUSE_LEFT_DOWN)
{
    CGEventRef event = context;
    CGPoint point = CGEventGetLocation(event);
    CFRelease(event);

    debug("%s: %.2f, %.2f\n", __FUNCTION__, point.x, point.y);
    mouse_left_down = true;

    mouse_window = window_manager_find_window_at_point(&g_window_manager, point);
    if (!mouse_window) mouse_window = window_manager_focused_window(&g_window_manager);
    if (!mouse_window) return;

    mouse_window_frame = window_ax_frame(mouse_window);
    border_window_topmost(mouse_window, true);
}

static EVENT_CALLBACK(EVENT_HANDLER_MOUSE_LEFT_UP)
{
    CGEventRef event = context;
    CGPoint point = CGEventGetLocation(event);
    CFRelease(event);

    debug("%s: %.2f, %.2f\n", __FUNCTION__, point.x, point.y);
    mouse_left_down = false;
    if (!mouse_window) return;

    if (!__sync_bool_compare_and_swap(mouse_window->id_ptr, &mouse_window->id, &mouse_window->id)) {
        debug("%s: %d has been marked invalid by the system, ignoring event..\n", __FUNCTION__, mouse_window->id);
        mouse_window = NULL;
        return;
    }

    CGRect frame = window_ax_frame(mouse_window);
    float dx = frame.origin.x - mouse_window_frame.origin.x;
    float dy = frame.origin.y - mouse_window_frame.origin.y;
    float dw = frame.size.width - mouse_window_frame.size.width;
    float dh = frame.size.height - mouse_window_frame.size.height;

    if ((dx != 0.0f || dy != 0.0f) && (dw != 0.0f || dh != 0.0f)) {
        uint8_t direction = 0;
        if (dx != 0.0f) direction |= HANDLE_LEFT;
        if (dy != 0.0f) direction |= HANDLE_TOP;
        window_manager_resize_window_relative(&g_window_manager, mouse_window, direction, dx, dy);
    }

    if (dw != 0.0f || dh != 0.0f) {
        uint8_t direction = 0;
        if (dw != 0.0f) direction |= HANDLE_RIGHT;
        if (dh != 0.0f) direction |= HANDLE_BOTTOM;
        window_manager_resize_window_relative(&g_window_manager, mouse_window, direction, dw, dh);
    }

    border_window_topmost(mouse_window, false);
    mouse_window = NULL;
}

static EVENT_CALLBACK(EVENT_HANDLER_MOUSE_MOVED)
{
    CGEventRef event = context;
    CGPoint point = CGEventGetLocation(event);
    uint64_t event_time = CGEventGetTimestamp(event);
    CFRelease(event);

    float delta = ((float)event_time - mouse_moved_time) * (1.0f / 1E6);
    if (delta < 35.0f) return;

    if (g_window_manager.ffm_mode != FFM_DISABLED) {
        struct ax_window *window = window_manager_find_window_at_point(&g_window_manager, point);
        if (window && window->id != g_window_manager.focused_window_id && window_is_standard(window)) {
            if (g_window_manager.ffm_mode == FFM_AUTOFOCUS) {
                window_manager_focus_window_without_raise(window->id);
            } else {
                window_manager_focus_window_with_raise(window->id);
            }
        }
    }

    mouse_moved_time = event_time;
}

static EVENT_CALLBACK(EVENT_HANDLER_MENU_BAR_HIDDEN_CHANGED)
{
    debug("%s:\n", __FUNCTION__);
    space_manager_mark_spaces_invalid(&g_space_manager);
}

static EVENT_CALLBACK(EVENT_HANDLER_DAEMON_MESSAGE)
{
    FILE *rsp = fdopen(param1, "w");
    if (!rsp) goto fderr;

    // debug("%s: msg '%s'\n", __FUNCTION__, context);
    handle_message(rsp, context);

    fflush(rsp);
    fclose(rsp);

fderr:
    socket_close(param1);
    free(context);
}
