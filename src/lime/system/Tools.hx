package lime.system;

import lime._internal.backend.native.NativeCFFI;

/**
	Access operating system level settings and operations.
**/
#if !lime_debug
@:fileXml('tags="haxe,release"')
@:noDebug
#end
@:access(lime._internal.backend.native.NativeCFFI)
@:access(lime.system.Display)
@:access(lime.system.DisplayMode)
class Tools
{
	/**
		The number of milliseconds and microseconds since the application was initialized.
	**/
	public static function getAccurateTimer():Float
	{
		#if flash
		return flash.Lib.getTimer();
		#elseif ((js && !nodejs) || electron)
		return js.Browser.window.performance.now();
		#elseif (lime_cffi && !macro)
		return NativeCFFI.lime_system_get_timer();
		#elseif cpp
		return untyped __global__.__time_stamp() * 1000;
		#elseif sys
		return Sys.time() * 1000;
		#else
		return 0;
		#end
	}
}