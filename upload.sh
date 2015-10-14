#!/bin/bash

key=$(cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 32 | head -n 1)

echo $key

#curl -i -F "key=$key" -F "acl=private" -F "AWSAccessKeyId=AKIAIT4VU4B7G2LQYKZQ" -F "policy=eyAiZXhwaXJhdGlvbiI6ICIyMDIwLTEyLTAxVDEyOjAwOjAwLjAwMFoiLAogICAgICAgICAgICAiY29uZGl0aW9ucyI6IFsKICAgICAgICAgICAgeyJidWNrZXQiOiAiZ2lmYWZmZSJ9LAogICAgICAgICAgICBbInN0YXJ0cy13aXRoIiwgIiRrZXkiLCAiIl0sCiAgICAgICAgICAgIHsiYWNsIjogInByaXZhdGUifSwKCSAgICB7InN1Y2Nlc3NfYWN0aW9uX3N0YXR1cyI6ICIyMDAifSwKICAgICAgICAgICAgWyJzdGFydHMtd2l0aCIsICIkQ29udGVudC1UeXBlIiwgIiJdLAogICAgICAgICAgICBbImNvbnRlbnQtbGVuZ3RoLXJhbmdlIiwgMCwgNTI0Mjg4MDAwXQogICAgICAgICAgICBdCiAgICAgICAgICB9" -F "success_action_status=200" -F "signature=mk9t/U/wRN4/uU01mXfeTe2Kcoc=" -F "Content-Type=image/gif" -F "file=@$file" https://gifaffe.s3.amazonaws.com/
