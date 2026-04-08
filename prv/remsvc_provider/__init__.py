"""
remsvc_provider
===============
Apache Airflow provider for RemSvc remote execution via gRPC.
"""

from __future__ import annotations


def get_provider_info() -> dict:
    """Required by Airflow's provider discovery mechanism."""
    return {
        "package-name": "airflow-provider-remsvc",
        "name": "RemSvc",
        "description": "Remote command execution for Apache Airflow via RemSvc gRPC.",
        "versions": ["1.0.0"],
        "min-airflow-version": "3.1.0",
        "cli": [],
    }
