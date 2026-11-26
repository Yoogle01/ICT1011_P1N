The daemon trains on exactly ONE photo, at the path pointed to by the
SMARTLOCK_REF_PHOTO environment variable (default:
./reference_photos/owner_photo/front.jpeg, relative to
face_presence_daemon.py).

owner_photo/front.jpeg is already included and used by default. The
left/right/back angle photos that used to sit alongside it were deleted
from this folder - they were never read by the daemon anyway (an earlier
design trained on multiple angles; the current one only ever uses this
single photo). If you want a different reference photo, either replace
front.jpeg directly or point SMARTLOCK_REF_PHOTO at a new photo of your
own.

(The original full set of angle photos still exists separately, at
../../../TinyCircuit_pictures/owner_photo/, for other uses elsewhere
in the project - that folder was left untouched.)

If SMARTLOCK_REF_PHOTO points at a missing file, or a photo with no
detectable face, the daemon logs a warning and fails-secure (treats
every frame as "not the owner") rather than defaulting to "always
present".
