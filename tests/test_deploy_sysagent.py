from __future__ import annotations

import unittest
from unittest.mock import Mock, patch

import deploy_sysagent


class DeploySysAgentTests(unittest.TestCase):
    def test_ftp_connect_failure_preserves_original_error(self) -> None:
        ftp = Mock()
        ftp.sock = None
        ftp.connect.side_effect = TimeoutError("timed out")

        with patch("deploy_sysagent.ftplib.FTP", return_value=ftp):
            with self.assertRaisesRegex(TimeoutError, "timed out"):
                deploy_sysagent.upload_and_rename("192.0.2.1", 6001)

        ftp.quit.assert_not_called()


if __name__ == "__main__":
    unittest.main()
