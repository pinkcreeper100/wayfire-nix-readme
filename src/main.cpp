#include "core.hpp"
#include "output.hpp"
#include <stdio.h>


#include <signal.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <execinfo.h>
#include <cxxabi.h>


#define Crash 101
#define max_frames 100

void print_trace() {
    std::cout << "stack trace:\n";

    // storage array for stack trace address data
    void* addrlist[max_frames + 1];

    // retrieve current stack addresses
    int addrlen = backtrace(addrlist, sizeof(addrlist) / sizeof(void*));

    if (addrlen == 0) {
        std::cout << "<empty, possibly corrupt>\n";
        return;
    }

    // resolve addresses into strings containing "filename(function+address)",
    // this array must be free()-ed
    char** symbollist = backtrace_symbols(addrlist, addrlen);

    //allocate string which will be filled with
    //the demangled function name
    size_t funcnamesize = 256;
    char* funcname = (char*)malloc(funcnamesize);

    // iterate over the returned symbol lines. skip the first, it is the
    // address of this function.
    for(int i = 1; i < addrlen; i++) {
        char *begin_name = 0, *begin_offset = 0, *end_offset = 0;

        // find parentheses and +address offset surrounding the mangled name:
        // ./module(function+0x15c)[0x8048a6d]
        for(char* p = symbollist[i]; *p; ++p) {
            if(*p == '(')
                begin_name = p;
            else if(*p == '+')
                begin_offset = p;
            else if(*p == ')' && begin_offset){
                end_offset = p;
                break;
            }
        }

        if(begin_name && begin_offset && end_offset
                &&begin_name < begin_offset)
        {
            *begin_name++ = '\0';
            *begin_offset++ = '\0';
            *end_offset = '\0';

            // mangled name is now in[begin_name, begin_offset) and caller
            // offset in [begin_offset, end_offset). now apply
            // __cxa_demangle():

            int status;
            char *ret = abi::__cxa_demangle(begin_name,
                    funcname, &funcnamesize, &status);
            if(status == 0) {
                funcname = ret;// use possibly realloc()-ed string
                printf("%s:%s+%s\n", symbollist[i], funcname, begin_offset);
            }
            else{
                // demangling failed. Output function name as a C function with
                // no arguments.
                printf("%s:%s()+%s\n",
                        symbollist[i], begin_name, begin_offset);
            }
        }
        else
        {
            // couldn't parse the line? print the whole line.
            printf("%s\n",symbollist[i]);
        }
    }

    free(funcname);
    free(symbollist);

    exit(-1);
}



void signalHandle(int sig) {
    switch(sig) {
        case SIGINT:                 // if interrupted, then
            std::cout << "EXITING BECAUSE OF SIGINT" << std::endl;
            break;

        case SIGUSR1:
            std::cout << "SIGUSR1" << std::endl;
            if(!core)
                std::cout << "in main process" << std::endl;
            else {
                delete core;
                std::exit(0);
            }

            break;

        default: // program crashed, so restart core
            std::cout << "Crash Detected!!!!!!" << std::endl;
            print_trace();
            break;
    }
}

void on_activate() {
    std::cout << "001010011341" << std::endl;
    core->for_each_output([] (Output *o) {o->activate();});
}

void on_deactivate() {
    core->for_each_output([] (Output *o) {o->deactivate();});
}

bool keyboard_key(wlc_handle view, uint32_t time,
        const struct wlc_modifiers *modifiers, uint32_t key,
        enum wlc_key_state state) {
    uint32_t sym = wlc_keyboard_get_keysym_for_key(key, NULL);

    Output *output = core->get_active_output();
    bool grabbed = output->input->process_key_event(sym, modifiers->mods, state);

    if (output->should_redraw())
        wlc_output_schedule_render(output->get_handle());

    return grabbed;
}

bool pointer_button(wlc_handle view, uint32_t time,
        const struct wlc_modifiers *modifiers, uint32_t button,
        enum wlc_button_state state, const struct wlc_point *position) {

    assert(core);

    Output *output = core->get_active_output();
    bool grabbed = output->input->process_button_event(button, modifiers->mods, state, *position);
    if (output->should_redraw())
        wlc_output_schedule_render(output->get_handle());

    return grabbed;
}

bool pointer_motion(wlc_handle view, uint32_t time, const struct wlc_point *position) {
    assert(core);
    wlc_pointer_set_position(position);

    auto output = core->get_active_output();
    bool grabbed = output->input->process_pointer_motion_event(*position);

    if (output->should_redraw())
        wlc_output_schedule_render(output->get_handle());

    return grabbed;
}

bool view_created(wlc_handle view) {
    assert(core);
    core->add_view(view);
    return true;
}

void view_destroyed(wlc_handle view) {
    assert(core);

    core->rem_view(view);
    core->focus_view(core->get_active_output()->get_active_view());
}

void view_focus(wlc_handle view, bool focus) {
    wlc_view_set_state(view, WLC_BIT_ACTIVATED, focus);
    wlc_view_bring_to_front(view);
}

void view_request_resize(wlc_handle view, uint32_t edges, const struct wlc_point *origin) {
    SignalListenerData data;

    auto v = core->find_view(view);
    if (!v)
       return;

    data.push_back((void*)(&v));
    data.push_back((void*)(origin));

    v->output->signal->trigger_signal("resize-request", data);
}


void view_request_move(wlc_handle view, const struct wlc_point *origin) {
    SignalListenerData data;

    auto v = core->find_view(view);
    if (!v)
       return;

    data.push_back((void*)(&v));
    data.push_back((void*)(origin));

    v->output->signal->trigger_signal("move-request", data);
}

void output_pre_paint(wlc_handle output) {
    assert(core);

    /* TODO: format this */
    Output *o;
    if (!(o = core->get_output(output))) {
        core->add_output(output);
        o = core->get_output(output);
        o->render->load_context();
    } else {
        o->render->paint();
    }
}

void output_post_paint(wlc_handle output) {
    assert(core);

    auto o = core->get_output(output);
    if (!o) return;

    o->render->post_paint();
    o->hook->run_hooks();
    if (o->should_redraw()) {
        wlc_output_schedule_render(output);
        o->for_each_view([] (View v) {
            wlc_surface_flush_frame_callbacks(v->get_surface());
        });
    }
}

bool output_created(wlc_handle output) {
    std::cout << "output created" << std::endl;
    //core->add_output(output);
    return true;
}

/* TODO: handle this, move all views from this output and disable it */
void output_destroyed(wlc_handle output) {
}

void log(wlc_log_type type, const char *msg) {
    std::cout << "wlc: " << msg << std::endl;
}

void view_request_geometry(wlc_handle view, const wlc_geometry *g) {
    auto v = core->find_view(view);
    if (!v) return;

    /* TODO: add pending changes for views that are not visible */
    if(v->is_visible() || v->default_mask == 0) {
        v->set_geometry(g->origin.x, g->origin.y, g->size.w, g->size.h);
        v->set_mask(v->output->viewport->get_mask_for_view(v));
    }
}

void view_request_state(wlc_handle view, wlc_view_state_bit state, bool toggle) {
    if(state == WLC_BIT_MAXIMIZED || state == WLC_BIT_FULLSCREEN)
        wlc_view_set_state(view, state, false);
    else
        wlc_view_set_state(view, state, toggle);
}

void output_focus(wlc_handle output, bool is_focused) {
//    if (is_focused)
//        core->focus_output(core->get_output(output));
}

void output_ctx_created(wlc_handle output) {
    Output *o = core->get_output(output);
    if (!o)
        return;

    std::cout << "output" << std::endl;

    if (o->render->ctx) {
        std::cout << "delete context" << std::endl;
        delete o->render->ctx;
    }

    std::cout << "here" << std::endl;
    o->render->load_context();
}

void output_ctx_destroyed(wlc_handle output) {
//    Output *o = core->get_output(output);
//    if (o) o->render->release_context();
}

void view_move_to_output(wlc_handle view, wlc_handle old, wlc_handle new_output) {
    core->move_view_to_output(core->find_view(view), core->get_output(old), core->get_output(new_output));
}

bool on_scroll(wlc_handle view, uint32_t time, const struct wlc_modifiers* mods,
        uint8_t axis, double amount[2]) {

    std::cout << "scrolling" << std::endl;
    auto output = core->get_active_output();
    if (output) {
        return output->input->process_scroll_event(mods->mods, amount);
    } else {
        return false;
    }
}

void view_pre_paint(wlc_handle v) {
    auto view = core->find_view(v);
    if (view && !view->destroyed) {
        wlc_geometry g;
        wlc_view_get_visible_geometry(v, &g);
    }
}

void readyyyy() {
    std::cout << "fhahdfasjflkadsjfalsjdflasjfla1234566" << std::endl;
}

int main(int argc, char *argv[]) {
    wlc_log_set_handler(log);

    signal(SIGINT, signalHandle);
    signal(SIGSEGV, signalHandle);
    signal(SIGFPE, signalHandle);
    signal(SIGILL, signalHandle);
    signal(SIGABRT, signalHandle);
    signal(SIGTRAP, signalHandle);

    wlc_set_view_created_cb       (view_created);
    wlc_set_view_destroyed_cb     (view_destroyed);
    wlc_set_view_focus_cb         (view_focus);
    wlc_set_view_move_to_output_cb(view_move_to_output);

    wlc_set_view_render_pre_cb(view_pre_paint);
    wlc_set_view_request_resize_cb(view_request_resize);
    wlc_set_view_request_move_cb(view_request_move);
    wlc_set_view_request_geometry_cb(view_request_geometry);
    wlc_set_view_request_state_cb(view_request_state);

    wlc_set_output_created_cb(output_created);
    wlc_set_output_destroyed_cb(output_destroyed);
    wlc_set_output_focus_cb(output_focus);

    wlc_set_output_render_pre_cb(output_pre_paint);
    wlc_set_output_render_post_cb(output_post_paint);

    wlc_set_output_context_created_cb(output_ctx_created);
    wlc_set_output_context_destroyed_cb(output_ctx_destroyed);

    wlc_set_keyboard_key_cb(keyboard_key);
    wlc_set_pointer_scroll_cb(on_scroll);
    wlc_set_pointer_button_cb(pointer_button);
    wlc_set_pointer_motion_cb(pointer_motion);

    wlc_set_compositor_ready_cb(readyyyy);

    core = new Core();
    core->init();

    if (!wlc_init2())
        return EXIT_FAILURE;

    wlc_run();
    return EXIT_SUCCESS;
}
