// [library:window] Thread-safe SDL event queue between the main thread and the game
// thread.

#include "miniwin.h"

#include <miniwin/miniwinapp.h>
#include <vector>

// SDL frees dynamically allocated event payloads (text input strings) once the
// original event goes out of scope on the main thread, so the queue keeps its own
// copy of the text and repoints the event at it when handing it out.
struct QueuedEvent {
	SDL_Event m_event;
	char m_text[64];
};

static SDL_Mutex* g_queueMutex;
static std::vector<QueuedEvent> g_queue;
static size_t g_queueHead;

static void EnsureQueue()
{
	if (!g_queueMutex) {
		g_queueMutex = SDL_CreateMutex();
	}
}

void MiniwinApp_PushEvent(const SDL_Event& p_event)
{
	EnsureQueue();
	SDL_LockMutex(g_queueMutex);

	QueuedEvent entry;
	entry.m_event = p_event;
	entry.m_text[0] = '\0';
	if (p_event.type == SDL_EVENT_TEXT_INPUT && p_event.text.text) {
		SDL_strlcpy(entry.m_text, p_event.text.text, sizeof(entry.m_text));
	}
	g_queue.push_back(entry);

	SDL_UnlockMutex(g_queueMutex);
}

bool MiniwinApp_PollEvent(SDL_Event& p_event)
{
	EnsureQueue();
	SDL_LockMutex(g_queueMutex);

	bool result = false;
	if (g_queueHead < g_queue.size()) {
		static char textBuffer[64];
		QueuedEvent& entry = g_queue[g_queueHead];
		p_event = entry.m_event;
		if (p_event.type == SDL_EVENT_TEXT_INPUT) {
			SDL_strlcpy(textBuffer, entry.m_text, sizeof(textBuffer));
			p_event.text.text = textBuffer;
		}
		g_queueHead++;
		result = true;
	}
	else if (!g_queue.empty()) {
		g_queue.clear();
		g_queueHead = 0;
	}

	SDL_UnlockMutex(g_queueMutex);
	return result;
}
