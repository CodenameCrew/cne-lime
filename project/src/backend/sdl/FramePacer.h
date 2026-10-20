#ifndef LIME_SDL_FRAME_PACER_H
#define LIME_SDL_FRAME_PACER_H


#include <SDL3/SDL.h>


namespace lime {


	class FramePacer {

		public:

			FramePacer ();
			~FramePacer ();

			void SetWindow (SDL_Window* sdlWindow);

			bool WaitFrameStart (Uint64 target);

			bool ShouldSwap (Uint64 now);

			void OnSwapEnd (Uint64 now);

			void OnFrameEnd (Uint64 frameEnd);

		private:

			void* impl;

	};


}


#endif // LIME_SDL_FRAME_PACER_H
