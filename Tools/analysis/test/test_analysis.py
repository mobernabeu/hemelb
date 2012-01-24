#!/usr/bin/env python
# encoding: utf-8
"""
test_analysis.py

Created by James Hetherington on 2012-01-23.
Copyright (c) 2012 __MyCompanyName__. All rights reserved.
"""

import os
import unittest

from .. import Analysis
import fixtures
import tempfile 

class TestAnalysis(unittest.TestCase):
	def setUp(self):
	    self.config={
	        'graphs': {'performance_versus_cores':fixtures.GraphConfig('performance_versus_cores')},
	        'reports': {'planck_performance':fixtures.ReportConfig('planck_performance')},
	        'results_path': fixtures.Results('cylinders').path,
	        'reports_path': tempfile.mkdtemp(),
	        'results': fixtures.ResultsConfig('example')
	    }
	def test_construct(self):
		a=Analysis(self.config)