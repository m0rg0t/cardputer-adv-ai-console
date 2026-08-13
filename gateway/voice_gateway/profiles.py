from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class VoiceProfile:
    id: str
    name: str
    folder: str
    instruction: str
    tags: tuple[str, ...]


PROFILES: dict[str, VoiceProfile] = {
    "default": VoiceProfile(
        "default",
        "Note",
        "",
        "Turn this into a concise, well-structured note.",
        ("voice-note",),
    ),
    "meeting": VoiceProfile(
        "meeting",
        "Meeting",
        "Meetings",
        "Create meeting notes with summary, decisions, and action items. "
        "Do not invent owners or deadlines.",
        ("voice-note", "meeting"),
    ),
    "idea": VoiceProfile(
        "idea",
        "Idea",
        "Ideas",
        "Structure the idea into problem, proposal, open questions, and next steps.",
        ("voice-note", "idea"),
    ),
    "task": VoiceProfile(
        "task",
        "Task",
        "Tasks",
        "Extract an actionable task checklist while preserving context and uncertainty.",
        ("voice-note", "task"),
    ),
}


def get_profile(profile_id: str) -> VoiceProfile:
    return PROFILES.get(profile_id, PROFILES["default"])
