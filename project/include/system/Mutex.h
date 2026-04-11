#ifndef LIME_SYSTEM_MUTEX_H
#define LIME_SYSTEM_MUTEX_H


namespace lime {


	class Mutex {


		public:

			Mutex ();
			~Mutex ();

			void Lock () const;
			bool TryLock () const;
			void Unlock () const;

		private:

			void* mutex;


	};


}


#endif