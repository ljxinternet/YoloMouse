#include <Snoopy/Constants.hpp>
#include <Snoopy/Inject/Injector.hpp>
#include <Dbghelp.h>
#include <TlHelp32.h>

namespace Snoopy
{
    // public
    //-------------------------------------------------------------------------
    Injector::Function::Function()
    {
    }

    // public
    //-------------------------------------------------------------------------
    Injector::Injector()
    {
        // init
        _Reset();

        // register functions
        _functions[FUNCTION_LOADLIBRARY].name = "LoadLibraryA";
        _functions[FUNCTION_FREELIBRARY].name = "FreeLibrary";
    }

    Injector::~Injector()
    {
        if( _process )
            Unload();
    }

    //-------------------------------------------------------------------------
    void Injector::SetNotifyName( const CHAR* name )
    {
        _functions[FUNCTION_NOTIFY].name = name;
    }

    //-------------------------------------------------------------------------
    Bool Injector::Load( DWORD process_id, const CHAR* dll_path )
    {
        xassert(_process == NULL);

        // open process
        _process = OpenProcess(
            PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,
            FALSE,
            process_id);

        // if opened
        if( _process )
        {
            // initialize symbol loader
            if(SymInitialize(_process, NULL, FALSE))
            {
                // load initial modules
                if( _LoadSymbolModule("kernel32.dll") )
                {
                    // allocate argument memory
                    _argument_memory = VirtualAllocEx(_process, NULL, INJECTOR_ARGUMENT_MEMORY, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);

                    // if allocated
                    if( _argument_memory )
                    {
                        // inject dll
                        if( _InjectDll(dll_path) )
                            return true;
                    }
                }
            }

            // fail
            Unload();
        }

        return false;
    }

    //-------------------------------------------------------------------------
    void Injector::Unload()
    {
        // if loaded
        if(_process)
        {
            // unload dll if successfully injected
            if( _inject_base )
                _CallFunction(FUNCTION_FREELIBRARY, (void*)_inject_base);

            // free argument memory
            if( _argument_memory )
                VirtualFreeEx(_process, _argument_memory, INJECTOR_ARGUMENT_MEMORY, MEM_RELEASE);

            // close symbol handler
            SymCleanup(_process);

            // close process
            CloseHandle(_process);

            // reset state
            _Reset();
        }
    }

    //-------------------------------------------------------------------------
    Bool Injector::CallNotify( const void* argument, ULong size )
    {
        xassert(_process);
        return _CallFunction(FUNCTION_NOTIFY, argument, size);
    }

    // private
    //-------------------------------------------------------------------------
    Bool Injector::_GetFunctionAddress( UHuge& address, const CHAR* name )
    {
        SYMBOL_INFO symbol = {0};

        // init parameters
        symbol.SizeOfStruct = sizeof(symbol);

        // get address of function
        if( !SymFromName(_process, name, &symbol) || symbol.Address == 0 )
            return false;

        // store address
        address = symbol.Address;
        return true;
    }

    //-------------------------------------------------------------------------
    Bool Injector::_InjectDll( const CHAR* dll_path )
    {
        CHAR   full_path[STRING_PATH_SIZE];
        CHAR*  pdll_name = NULL;
        xassert(_process);

        // get full dll path
        if(!GetFullPathNameA(dll_path, COUNT(full_path), full_path, &pdll_name))
            return false;

        // tell process to load dll
        if( !_CallFunction(FUNCTION_LOADLIBRARY, full_path, static_cast<ULong>(strlen(full_path) + 1)) )
            return false;

        // load symbols for new dll
        _inject_base = _LoadSymbolModule(pdll_name);
        if(_inject_base == 0)
            return false;

        return true;
    }

    //-------------------------------------------------------------------------
    Bool Injector::_CallFunction( FunctionId id, const void* argument, ULong size )
    {
        xassert(id < FUNCTION_COUNT);
        Function&   f = _functions[id];
        void*       remote_argument = 0;

        // if not loaded
        if( f.address == 0 )
        {
            xassert(f.name);

            // get address of function
            if( !_GetFunctionAddress(f.address, f.name) )
                return false;
        }

        // if argument is a pointer to an object
        if( size > 0 )
        {
            xassert(size <= INJECTOR_ARGUMENT_MEMORY);

            // write object to argument memory
            if(!WriteProcessMemory(_process, _argument_memory, argument, size, NULL))
                return false;

            // argument is pointer to argument memory
            remote_argument = _argument_memory;
        }
        // else argument is the provided value
        else
            remote_argument = const_cast<void*>(argument);

        // call function
        HANDLE remote_thread = CreateRemoteThread(
            _process,
            NULL,
            0,
            (LPTHREAD_START_ROUTINE)f.address,
            remote_argument,
            NULL,
            NULL);
        if( remote_thread == NULL )
            return false;

        // wait for completion
        Bool status = WaitForSingleObject(remote_thread, INJECTOR_FUNCTION_WAIT) == WAIT_OBJECT_0;

        // close thread object
        CloseHandle(remote_thread);

        return status;
    }

    //-------------------------------------------------------------------------
    DWORD64 Injector::_LoadSymbolModule( const CHAR* name )
    {
        MODULEENTRY32 me;
        DWORD64       base_address = 0;

        // create toolhelp snapshot
        HANDLE handle = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32, GetProcessId(_process) );

        // if created
        if( handle != INVALID_HANDLE_VALUE )
        {
            // init
            me.dwSize = sizeof(me);

            // walk modules
            if( Module32First(handle, &me) )
            {
                do
                {
                    // if matches
                    if( _stricmp(me.szModule, name) == 0 )
                    {
                        // load module into symbol loader
                        base_address = SymLoadModuleEx(
                            _process,
                            NULL,
                            me.szExePath,
                            me.szModule,
                            (DWORD64)me.modBaseAddr,
                            me.modBaseSize,
                            NULL,
                            0);

                        break;
                    }
                }
                while( Module32Next(handle, &me) );
            }

            // close toolhelp snapshot
            CloseHandle(handle);
        }

        return base_address;
    }

    //-------------------------------------------------------------------------
    void Injector::_Reset()
    {
        _process = NULL;
        _argument_memory = NULL;
        _inject_base = 0;

        for( Index i = 0; i < FUNCTION_COUNT; ++i )
            _functions[i].address = 0;
    }
}
